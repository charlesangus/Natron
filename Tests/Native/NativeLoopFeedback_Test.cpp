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

#include <functional>
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "IdentityHopTestEffect.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderContext.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

RectI
countingWindow()
{
    return RectI(0, 0, kCountingTestSize, kCountingTestSize);
}

std::vector<int>
bothPools()
{
    return std::vector<int>({ 1, 4 });
}

// node's RGBA over roi, rendered the way mode renders a frame.
bool
renderPlane(const NodePtr& node,
            const RectI& roi,
            RenderSchedulerModeEnum mode,
            bool byPassCache,
            RenderedPlane* plane)
{
    std::vector<RenderedPlane> planes;
    std::string error;

    if (!renderNodePlanesInMode(node, kTime, ViewIdx(0), 0, roi, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), mode, byPassCache, &planes, &error)) {
        ADD_FAILURE() << error;

        return false;
    }
    for (const RenderedPlane& candidate : planes) {
        if (candidate.layer == ImageLayerDesc::getRGBAComponents()) {
            *plane = candidate;

            return true;
        }
    }
    ADD_FAILURE() << "no RGBA plane was rendered";

    return false;
}

// The red value of the bottom-left pixel of node's output.
bool
renderedValue(const NodePtr& node,
              RenderSchedulerModeEnum mode,
              bool byPassCache,
              float* value)
{
    RenderedPlane plane;

    if (!renderPlane(node, countingWindow(), mode, byPassCache, &plane)) {
        return false;
    }
    if (plane.pixels.empty()) {
        ADD_FAILURE() << "no pixel was read back";

        return false;
    }
    *value = plane.pixels[0];

    return true;
}

void
expectSamePixels(const RenderedPlane& expected,
                 const RenderedPlane& actual,
                 const std::string& what)
{
    ASSERT_EQ(expected.window, actual.window) << what;
    ASSERT_EQ(expected.channels, actual.channels) << what;
    ASSERT_EQ(expected.pixels.size(), actual.pixels.size()) << what;
    const std::size_t nComps = expected.channels.empty() ? 1 : expected.channels.size();
    for (std::size_t i = 0; i < expected.pixels.size(); ++i) {
        if (expected.pixels[i] != actual.pixels[i]) {
            const std::size_t pixel = i / nComps;
            ADD_FAILURE() << what << ": pixel " << (expected.window.x1 + (int)(pixel % (std::size_t)expected.window.width())) << ","
                          << (expected.window.y1 + (int)(pixel / (std::size_t)expected.window.width())) << " channel " << (i % nComps)
                          << ": " << expected.pixels[i] << " != " << actual.pixels[i];

            return;
        }
    }
}

// The bounding box of every region the request pass rooted at root asks of node over roi.
RectD
requestedRegion(const NodePtr& root,
                const NodePtr& node,
                const RectI& roi)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameArgs(kTime,
                                       ViewIdx(0),
                                       true /*isRenderUserInteraction*/,
                                       false /*isSequential*/,
                                       abortInfo,
                                       root,
                                       0 /*textureIndex*/,
                                       root->getApp()->getTimeLine().get(),
                                       NodePtr(),
                                       false /*isAnalysis*/,
                                       false /*draftMode*/,
                                       RenderStatsPtr());
    EffectInstancePtr effect = root->getEffectInstance();
    const RectD canonical = roi.toCanonical_noClipping(0, effect->getTimeInvariantMetadata()->getPixelAspectRatio(-1));
    FrameRequestMap request;
    RectD region;
    bool any = false;

    EXPECT_NE(eStatusFailed, EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0, canonical, root, request));
    for (FrameRequestMap::const_iterator it = request.begin(); it != request.end(); ++it) {
        if (it->first.node != node) {
            continue;
        }
        for (NodeFrameViewRequestData::const_iterator fv = it->second->frames.begin(); fv != it->second->frames.end(); ++fv) {
            const RectD& finalRoi = fv->second.finalData.finalRoi;
            if (!any) {
                region = finalRoi;
                any = true;
            } else {
                region.merge(finalRoi);
            }
        }
    }
    EXPECT_TRUE(any) << "the request pass asks nothing of " << node->getScriptName();

    return region;
}

class BytesBudgetGuard {
public:
    explicit BytesBudgetGuard(std::size_t budget)
        : _scheduler(appPTR->getRenderScheduler())
        , _saved(_scheduler->getBytesBudget())
    {
        _scheduler->setBytesBudgetForTests(budget);
    }

    ~BytesBudgetGuard()
    {
        _scheduler->setBytesBudgetForTests(_saved);
    }

    BytesBudgetGuard(const BytesBudgetGuard&) = delete;
    BytesBudgetGuard& operator=(const BytesBudgetGuard&) = delete;

private:
    RenderScheduler* _scheduler;
    std::size_t _saved;
};
} // namespace

class NativeLoopFeedbackTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
    }

    virtual void TearDown() OVERRIDE
    {
        CountingTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createConstant(double value)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        KnobColorPtr color = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kConstantParamColor)) : KnobColorPtr();

        EXPECT_TRUE(bool(color));
        if (!color) {
            return NodePtr();
        }
        for (int i = 0; i < color->getDimension(); ++i) {
            color->setValue(value, ViewSpec::all(), i);
        }

        return node;
    }

    // A Constant whose value is the frame number.
    NodePtr createFrameRamp()
    {
        NodePtr node = createConstant(0.);
        KnobColorPtr color = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kConstantParamColor)) : KnobColorPtr();

        if (!color) {
            return NodePtr();
        }
        for (int frame = -10; frame <= 10; ++frame) {
            for (int i = 0; i < color->getDimension(); ++i) {
                color->setValueAtTime((double)frame, (double)frame, ViewSpec::all(), i);
            }
        }

        return node;
    }

    NodePtr createCounting(double value)
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));

        EXPECT_TRUE(node && setDoubleKnob(node, "value", value));

        return node;
    }

    NodePtr createLoopBegin()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    }

    static bool setLoopName(const NodePtr& end,
                            const std::string& name)
    {
        KnobStringPtr knob = end ? std::dynamic_pointer_cast<KnobString>(end->getKnobByName(kLoopEndParamName)) : KnobStringPtr();

        if (!knob) {
            return false;
        }
        knob->setValue(name);

        return knob->getValue() == name;
    }

    NodePtr createFeedbackEnd(int iterations)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));

        EXPECT_TRUE(node && setIntKnob(node, kLoopEndParamIterations, iterations));
        EXPECT_TRUE(node && setChoiceKnob(node, kLoopEndParamMode, kLoopEndParamModeFeedback));

        return node;
    }

    static bool setIntKnob(const NodePtr& node,
                           const char* name,
                           int value)
    {
        KnobIntPtr knob = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(name));

        if (!knob) {
            return false;
        }
        knob->setValue(value);

        return true;
    }

    static bool setDoubleKnob(const NodePtr& node,
                              const char* name,
                              double value)
    {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));

        if (!knob) {
            return false;
        }
        knob->setValue(value, ViewSpec::all(), 0);

        return true;
    }

    static bool setBoolKnob(const NodePtr& node,
                            const char* name,
                            bool value)
    {
        KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(name));

        if (!knob) {
            return false;
        }
        knob->setValue(value);

        return knob->getValue() == value;
    }

    static bool setChoiceKnob(const NodePtr& node,
                              const char* name,
                              const std::string& id)
    {
        KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(name));

        if (!knob) {
            return false;
        }
        knob->setValueFromID(id, 0);

        return knob->getActiveEntry().id == id;
    }

    static bool setForceCaching(const NodePtr& node)
    {
        KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName("forceCaching"));

        if (!knob) {
            return false;
        }
        knob->setValue(true);

        return node->isForceCachingEnabled();
    }

    // Every node keeps its output in the cache, so a hash an edit fails to move shows as a stale image.
    void cacheEverything(const std::vector<NodePtr>& nodes)
    {
        for (const NodePtr& node : nodes) {
            // The counting nodes keep their output only through their own knob.
            if (node->getKnobByName("cacheOutput")) {
                EXPECT_TRUE(setBoolKnob(node, "cacheOutput", true));
            } else {
                EXPECT_TRUE(setForceCaching(node)) << node->getScriptName();
            }
        }
    }

    // Both schedulers, rendering through the cache, give expected at the bottom-left pixel, and
    // so does a render bypassing it.
    void expectValue(const NodePtr& node,
                     float expected,
                     const std::string& what)
    {
        float value = -1.f;
        ASSERT_TRUE(renderedValue(node, eRenderSchedulerModeLegacy, false, &value)) << what;
        EXPECT_EQ(expected, value) << what << " (legacy, cached)";
        ASSERT_TRUE(renderedValue(node, eRenderSchedulerModeTaskGraph, false, &value)) << what;
        EXPECT_EQ(expected, value) << what << " (task graph, cached)";
        ASSERT_TRUE(renderedValue(node, eRenderSchedulerModeLegacy, true, &value)) << what;
        EXPECT_EQ(expected, value) << what << " (legacy, uncached)";
    }

    void expectBothWays(const NodePtr& node,
                        const RectI& roi,
                        std::vector<int>* unplanned = 0)
    {
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), 0, roi, bothPools(), std::function<void()>(), 0.f, unplanned);
        EXPECT_FALSE(m.any) << describe(m);
    }

    // Sets, on end's Feedback LoopBegins, whether begin feeds back.
    static bool setBeginFeedsBack(const NodePtr& end,
                                  const NodePtr& begin,
                                  bool feedsBack)
    {
        LoopEnd* effect = end ? dynamic_cast<LoopEnd*>(end->getEffectInstance().get()) : 0;

        if (!effect || !begin) {
            return false;
        }
        effect->setFeedsBack(begin, feedsBack);

        return effect->feedsBack(begin) == feedsBack;
    }

    static bool isKnobSecret(const NodePtr& node,
                             const char* name)
    {
        KnobIPtr knob = node ? node->getKnobByName(name) : KnobIPtr();

        EXPECT_TRUE(bool(knob)) << name;

        return knob && knob->getIsSecret();
    }

    // Two LoopBegins, from constants 1 and 10, summed by the body into a LoopEnd of four
    // iterations in Feedback mode; only the first LoopBegin feeds back.
    struct SharedLoop {
        NodePtr first;
        NodePtr second;
        NodePtr sum;
        NodePtr end;
    };

    bool createSharedLoop(SharedLoop* loop)
    {
        NodePtr one = createConstant(1.);
        NodePtr ten = createConstant(10.);
        loop->first = createLoopBegin();
        loop->second = createLoopBegin();
        loop->sum = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
        loop->end = createFeedbackEnd(4);

        return one && ten && loop->first && loop->second && loop->sum && loop->end && setDoubleKnob(loop->sum, "value", 0.) && setBeginFeedsBack(loop->end, loop->second, false) && loop->first->connectInput(one, 0) && loop->second->connectInput(ten, 0) && loop->sum->connectInput(loop->first, 0) && loop->sum->connectInput(loop->second, 1) && loop->end->connectInput(loop->sum, 0);
    }

    // Asks end for its route, which posts the loop's error, and returns the message posted.
    static QString loopErrorOf(const NodePtr& end)
    {
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -3;
        EXPECT_TRUE(end->getEffectInstance()->isIdentity_public(false, 0, kTime, RenderScale::identity, countingWindow(), ViewIdx(0), &inputTime, &inputView, &inputNb));
        EXPECT_EQ(-1, inputNb) << "an invalid loop is black";
        QString message;
        int type = -1;
        end->getPersistentMessage(&message, &type, false);
        EXPECT_EQ((int)eMessageTypeError, type) << message.toStdString();

        return message;
    }

    NodePtr createParitySource(const std::string& components)
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(node));
        if (node) {
            setParitySourceComponents(node, components);
        }

        return node;
    }
};

// A 3x3 minimum applied five times is the 11x11 minimum.
TEST_F(NativeLoopFeedbackTest, FiveOnePixelErodesAreAFivePixelErode)
{
    NodePtr source = createParitySource("rgba");
    NodePtr begin = createLoopBegin();
    NodePtr erode = createNode(QString::fromUtf8(PLUGINID_NATRON_ERODE));
    NodePtr end = createFeedbackEnd(5);
    NodePtr reference = createNode(QString::fromUtf8(PLUGINID_NATRON_ERODE));
    ASSERT_TRUE(source && begin && erode && end && reference);
    ASSERT_TRUE(setKnobValues(erode, kErodeDilateParamSize, { 1., 1. }));
    ASSERT_TRUE(setKnobValues(reference, kErodeDilateParamSize, { 5., 5. }));
    ASSERT_TRUE(begin->connectInput(source, 0));
    ASSERT_TRUE(erode->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(erode, 0));
    ASSERT_TRUE(reference->connectInput(source, 0));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const RectI roi(window.x1 + 8, window.y1 + 8, window.x2 - 8, window.y2 - 8);
    ASSERT_FALSE(roi.isNull());
    RenderedPlane expected, actual;
    ASSERT_TRUE(renderPlane(reference, roi, eRenderSchedulerModeLegacy, true, &expected));
    ASSERT_TRUE(renderPlane(end, roi, eRenderSchedulerModeLegacy, true, &actual));
    expectSamePixels(expected, actual, "legacy");
    ASSERT_TRUE(renderPlane(end, roi, eRenderSchedulerModeTaskGraph, true, &actual));
    expectSamePixels(expected, actual, "task graph");
    expectBothWays(end, roi);
}

// Under the impulse filter a 1 px translate is exact, so ten of them are a 10 px translate.
TEST_F(NativeLoopFeedbackTest, TenOnePixelTranslatesAreATenPixelTranslate)
{
    NodePtr source = createParitySource("rgba");
    NodePtr begin = createLoopBegin();
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM));
    NodePtr end = createFeedbackEnd(10);
    NodePtr reference = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM));
    ASSERT_TRUE(source && begin && transform && end && reference);
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamTranslate, { 1., 0. }));
    ASSERT_TRUE(setKnobValue(transform, kResamplerParamFilterType, std::string("impulse")));
    ASSERT_TRUE(setKnobValues(reference, kTransformNodeParamTranslate, { 10., 0. }));
    ASSERT_TRUE(setKnobValue(reference, kResamplerParamFilterType, std::string("impulse")));
    ASSERT_TRUE(begin->connectInput(source, 0));
    ASSERT_TRUE(transform->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(transform, 0));
    ASSERT_TRUE(reference->connectInput(source, 0));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const RectI roi(window.x1 + 12, window.y1 + 2, window.x2 - 2, window.y2 - 2);
    ASSERT_FALSE(roi.isNull());
    RenderedPlane expected, actual;
    ASSERT_TRUE(renderPlane(reference, roi, eRenderSchedulerModeLegacy, true, &expected));
    ASSERT_TRUE(renderPlane(end, roi, eRenderSchedulerModeLegacy, true, &actual));
    expectSamePixels(expected, actual, "legacy");
    ASSERT_TRUE(renderPlane(end, roi, eRenderSchedulerModeTaskGraph, true, &actual));
    expectSamePixels(expected, actual, "task graph");
    expectBothWays(end, roi);
}

// What the loop asks of its input, through n iterations of an r px erode, is what one n·r px
// erode asks.
TEST_F(NativeLoopFeedbackTest, TheRegionAskedOfTheInputGrowsByEveryIteration)
{
    const int iterations = 5;
    const int radius = 2;
    NodePtr source = createParitySource("rgba");
    NodePtr begin = createLoopBegin();
    NodePtr erode = createNode(QString::fromUtf8(PLUGINID_NATRON_ERODE));
    NodePtr end = createFeedbackEnd(iterations);
    NodePtr reference = createNode(QString::fromUtf8(PLUGINID_NATRON_ERODE));
    ASSERT_TRUE(source && begin && erode && end && reference);
    ASSERT_TRUE(setKnobValues(erode, kErodeDilateParamSize, { (double)radius, (double)radius }));
    ASSERT_TRUE(setKnobValues(reference, kErodeDilateParamSize, { (double)(iterations * radius), (double)(iterations * radius) }));
    ASSERT_TRUE(begin->connectInput(source, 0));
    ASSERT_TRUE(erode->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(erode, 0));
    ASSERT_TRUE(reference->connectInput(source, 0));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const int margin = iterations * radius + 4;
    const RectI roi(window.x1 + margin, window.y1 + margin, window.x2 - margin, window.y2 - margin);
    ASSERT_FALSE(roi.isNull());
    const RectD looped = requestedRegion(end, source, roi);
    const RectD direct = requestedRegion(reference, source, roi);
    EXPECT_EQ(direct.x1, looped.x1);
    EXPECT_EQ(direct.y1, looped.y1);
    EXPECT_EQ(direct.x2, looped.x2);
    EXPECT_EQ(direct.y2, looped.y2);
    EXPECT_GE(looped.width(), (double)(roi.width() + 2 * iterations * radius));
}

// Single Pass at iteration 2 of 5 is the output of a 3-iteration loop.
TEST_F(NativeLoopFeedbackTest, SinglePassIsTheLoopCutShort)
{
    NodePtr source = createParitySource("rgba");
    NodePtr begin = createLoopBegin();
    NodePtr erode = createNode(QString::fromUtf8(PLUGINID_NATRON_ERODE));
    NodePtr end = createFeedbackEnd(5);
    ASSERT_TRUE(source && begin && erode && end);
    ASSERT_TRUE(setKnobValues(erode, kErodeDilateParamSize, { 1., 1. }));
    ASSERT_TRUE(begin->connectInput(source, 0));
    ASSERT_TRUE(erode->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(erode, 0));
    ASSERT_TRUE(setBoolKnob(end, kLoopEndParamSinglePass, true));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamSinglePassIteration, 2));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const RectI roi(window.x1 + 4, window.y1 + 4, window.x2 - 4, window.y2 - 4);
    RenderedPlane singlePass, taskGraph, threeIterations;
    ASSERT_TRUE(renderPlane(end, roi, eRenderSchedulerModeLegacy, true, &singlePass));
    ASSERT_TRUE(renderPlane(end, roi, eRenderSchedulerModeTaskGraph, true, &taskGraph));
    expectSamePixels(singlePass, taskGraph, "task graph");
    expectBothWays(end, roi);

    ASSERT_TRUE(setBoolKnob(end, kLoopEndParamSinglePass, false));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamIterations, 3));
    ASSERT_TRUE(renderPlane(end, roi, eRenderSchedulerModeLegacy, true, &threeIterations));
    expectSamePixels(threeIterations, singlePass, "single pass");
}

// The body reads no loop variable, yet each iteration is its own render of it.
TEST_F(NativeLoopFeedbackTest, ABodyReadingNoLoopVariableRendersEveryIteration)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin();
    NodePtr counting = createCounting(1.);
    NodePtr end = createFeedbackEnd(4);
    ASSERT_TRUE(constant && begin && counting && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(counting->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(counting, 0));

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, eRenderSchedulerModeLegacy, true, &value));
    EXPECT_EQ(5.f, value);
    EXPECT_EQ(4, CountingTestRegistry::renders(counting));
    ASSERT_TRUE(renderedValue(end, eRenderSchedulerModeTaskGraph, true, &value));
    EXPECT_EQ(5.f, value);
    expectBothWays(end, countingWindow());
}

// A legacy render split into several rectangles, by what the cache already holds of the LoopEnd,
// runs the chain once, not once per rectangle.
TEST_F(NativeLoopFeedbackTest, ATiledLegacyRenderRunsTheChainOnce)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin();
    NodePtr counting = createCounting(1.);
    NodePtr end = createFeedbackEnd(4);
    ASSERT_TRUE(constant && begin && counting && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(counting->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(counting, 0));
    ASSERT_TRUE(setForceCaching(end));

    CountingTestRegistry::reset();
    RenderedPlane centre;
    ASSERT_TRUE(renderPlane(end, RectI(8, 8, 24, 24), eRenderSchedulerModeLegacy, false, &centre));
    EXPECT_EQ(4, CountingTestRegistry::renders(counting)) << "centre";

    CountingTestRegistry::reset();
    RenderedPlane full;
    ASSERT_TRUE(renderPlane(end, countingWindow(), eRenderSchedulerModeLegacy, false, &full));
    EXPECT_EQ(4, CountingTestRegistry::renders(counting)) << "around the cached centre";
    const std::size_t nComps = full.channels.empty() ? 1 : full.channels.size();
    ASSERT_EQ((std::size_t)kCountingTestSize * kCountingTestSize * nComps, full.pixels.size());
    for (std::size_t i = 0; i < full.pixels.size(); i += nComps) {
        ASSERT_EQ(5.f, full.pixels[i]) << "pixel " << (i / nComps);
    }
}

// A single pass at iteration k renders the body k + 1 times: each iteration before it once.
TEST_F(NativeLoopFeedbackTest, ASinglePassRendersEachIterationBeforeItOnce)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin();
    NodePtr counting = createCounting(1.);
    NodePtr end = createFeedbackEnd(6);
    ASSERT_TRUE(constant && begin && counting && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(counting->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(counting, 0));
    ASSERT_TRUE(setBoolKnob(end, kLoopEndParamSinglePass, true));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamSinglePassIteration, 3));

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, eRenderSchedulerModeLegacy, true, &value));
    EXPECT_EQ(5.f, value);
    EXPECT_EQ(4, CountingTestRegistry::renders(counting));
}

// The iteration before is converted to the loop input's components, with a warning on the LoopBegin.
TEST_F(NativeLoopFeedbackTest, ABodyChangingComponentsIsConformedWithAWarning)
{
    NodePtr source = createParitySource("alpha");
    NodePtr begin = createLoopBegin();
    NodePtr counting = createCounting(1.);
    // Adds a different amount per channel, so that the final channels tell what fed the body.
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr end = createFeedbackEnd(3);
    ASSERT_TRUE(source && begin && counting && grade && end);
    KnobColorPtr gradeOffset = std::dynamic_pointer_cast<KnobColor>(grade->getKnobByName(kGradeParamOffset));
    ASSERT_TRUE(bool(gradeOffset));
    for (int i = 0; i < gradeOffset->getDimension(); ++i) {
        gradeOffset->setValue(10. * i, ViewSpec::all(), i);
    }
    ASSERT_TRUE(begin->connectInput(source, 0));
    ASSERT_TRUE(counting->connectInput(begin, 0));
    ASSERT_TRUE(grade->connectInput(counting, 0));
    ASSERT_TRUE(end->connectInput(grade, 0));

    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, eRenderSchedulerModeLegacy, true, &value));
    QString message;
    int messageType = -1;
    begin->getPersistentMessage(&message, &messageType, false);
    EXPECT_EQ((int)eMessageTypeWarning, messageType) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("converted"))) << message.toStdString();
    expectBothWays(end, countingWindow());

    setParitySourceComponents(source, "rgba");
    CountingTestRegistry::reset();
    ASSERT_TRUE(renderedValue(end, eRenderSchedulerModeLegacy, true, &value));
    std::cerr << "ABodyChangingComponents first RGBA render: body rendered " << CountingTestRegistry::renders(counting) << " times" << std::endl;

    // Tell a body that iterates wrongly from a loop input that is not RGBA.
    float sourceValue = -1.f;
    ASSERT_TRUE(renderedValue(source, eRenderSchedulerModeLegacy, true, &sourceValue));
    EXPECT_EQ(sourceValue + 3.f, value) << "after 3 feedback iterations: source " << sourceValue << ", rendered " << value;
    const EffectInstancePtr beginEffect = begin->getEffectInstance();
    const int beginComps = EffectInstance::getNCompsFromMetadata(*beginEffect->getTimeInvariantMetadata(), -1);
    EXPECT_EQ(4, beginComps) << "LoopBegin output components (nComps)";
    ImageLayerDesc beginLayer, beginPaired;
    EffectInstance::getLayersFromMetadata(*beginEffect->getTimeInvariantMetadata(), -1, &beginLayer, &beginPaired);
    EXPECT_TRUE(beginLayer == ImageLayerDesc::getRGBAComponents()) << "LoopBegin output layer " << beginLayer.getUserFacingLabel() << " (" << beginLayer.getNumComponents() << " comps)";

    RenderedPlane finalPlane;
    ASSERT_TRUE(renderPlane(end, countingWindow(), eRenderSchedulerModeLegacy, true, &finalPlane));
    ASSERT_GE(finalPlane.pixels.size(), (std::size_t)4);
    std::cerr << "ABodyChangingComponents final RGBA " << finalPlane.pixels[0] << " " << finalPlane.pixels[1] << " "
              << finalPlane.pixels[2] << " " << finalPlane.pixels[3] << " (source " << sourceValue << ")" << std::endl;
    EXPECT_EQ(sourceValue + 3.f, finalPlane.pixels[0]) << "R";
    EXPECT_EQ(sourceValue + 3.f + 10.f, finalPlane.pixels[1]) << "G";
    EXPECT_EQ(sourceValue + 3.f + 20.f, finalPlane.pixels[2]) << "B";
    // Grade leaves alpha alone by default, whatever its alpha offset.
    EXPECT_EQ(sourceValue + 3.f, finalPlane.pixels[3]) << "A";

    // The LoopBegin's metadata under iteration contexts 1 and 2.
    std::shared_ptr<LoopEnd> loopEnd = std::dynamic_pointer_cast<LoopEnd>(end->getEffectInstance());
    ASSERT_TRUE(bool(loopEnd));
    for (int k = 1; k <= 2; ++k) {
        RenderContextPtr iteration = LoopEnd::iterationContext(RenderContext::empty(), loopEnd->getLoopName(), k, 3, 0, 1, kTime);
        ActionContextScope contextScope(true, iteration);
        NodeMetadataConstPtr metadata = beginEffect->getMetadata(kTime, ViewIdx(0));
        ASSERT_TRUE(bool(metadata)) << "iteration " << k;
        const int comps = EffectInstance::getNCompsFromMetadata(*metadata, -1);
        ImageLayerDesc layer, paired;
        EffectInstance::getLayersFromMetadata(*metadata, -1, &layer, &paired);
        std::cerr << "ABodyChangingComponents LoopBegin metadata under iteration " << k << ": nComps " << comps << ", layer "
                  << layer.getUserFacingLabel().c_str() << " (" << layer.getNumComponents() << " comps)" << std::endl;
        EXPECT_EQ(4, comps) << "iteration " << k << " nComps";
        EXPECT_TRUE(layer == ImageLayerDesc::getRGBAComponents()) << "iteration " << k << " layer " << layer.getUserFacingLabel() << " (" << layer.getNumComponents() << " comps)";
    }

    // getPersistentMessage appends: read into an empty string, not the one holding the alpha warning.
    QString rgbaMessage;
    begin->getPersistentMessage(&rgbaMessage, &messageType, false);
    EXPECT_TRUE(rgbaMessage.isEmpty()) << rgbaMessage.toStdString();
}

// Iterating over frames, each iteration adds its frame onto the one before: a trail.
TEST_F(NativeLoopFeedbackTest, FramesLeaveATrail)
{
    NodePtr constant = createConstant(0.5);
    NodePtr begin = createLoopBegin();
    NodePtr merge = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    // Animated, so its cached images are told apart by frame.
    NodePtr frames = createFrameRamp();
    NodePtr end = createFeedbackEnd(1);
    ASSERT_TRUE(constant && begin && merge && frames && end);
    ASSERT_TRUE(setDoubleKnob(merge, "value", 0.));
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(merge->connectInput(begin, 0));
    ASSERT_TRUE(merge->connectInput(frames, 1));
    ASSERT_TRUE(end->connectInput(merge, 0));
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamIterateOver, kLoopEndParamIterateOverFrames));
    ASSERT_TRUE(setDoubleKnob(end, kLoopEndParamFramesBefore, 0.));
    ASSERT_TRUE(setDoubleKnob(end, kLoopEndParamFramesAfter, 2.));
    ASSERT_TRUE(setDoubleKnob(end, kLoopEndParamFrameStep, 1.));

    // The frames 1, 2 and 3 onto 0.5.
    expectValue(end, 6.5f, "frames 0 to 2");
    expectBothWays(end, countingWindow());

    // The frames 1 and 3.
    ASSERT_TRUE(setDoubleKnob(end, kLoopEndParamFrameStep, 2.));
    expectValue(end, 4.5f, "frame step 2");
    expectBothWays(end, countingWindow());
}

// A time offset in the body takes each iteration a frame further back: three iterations read
// the input three frames back.
TEST_F(NativeLoopFeedbackTest, ATimeOffsetInTheBodyAddsUpOverIterations)
{
    NodePtr ramp = createFrameRamp();
    NodePtr begin = createLoopBegin();
    NodePtr shift = createNode(QString::fromUtf8(kTestPluginIDTimeShiftIdentity));
    NodePtr counting = createCounting(10.);
    NodePtr end = createFeedbackEnd(3);
    ASSERT_TRUE(ramp && begin && shift && counting && end);
    ASSERT_TRUE(begin->connectInput(ramp, 0));
    ASSERT_TRUE(shift->connectInput(begin, 0));
    ASSERT_TRUE(counting->connectInput(shift, 0));
    ASSERT_TRUE(end->connectInput(counting, 0));

    expectValue(end, (float)(kTime - 3.) + 30.f, "three iterations");
    expectBothWays(end, countingWindow());
}

// Every edit that changes what an iteration renders reaches the cached loop, in both schedulers.
TEST_F(NativeLoopFeedbackTest, EditsReachTheCachedLoop)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin();
    NodePtr body = createCounting(1.);
    NodePtr merge = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    NodePtr outside = createCounting(2.);
    NodePtr side = createCounting(5.);
    NodePtr end = createFeedbackEnd(3);
    ASSERT_TRUE(constant && begin && body && merge && outside && side && end);
    ASSERT_TRUE(setLoopName(end, "fb"));
    ASSERT_TRUE(setDoubleKnob(merge, "value", 0.));
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(body->connectInput(begin, 0));
    ASSERT_TRUE(merge->connectInput(body, 0));
    ASSERT_TRUE(merge->connectInput(outside, 1));
    ASSERT_TRUE(end->connectInput(merge, 0));
    cacheEverything({ constant, begin, body, merge, outside, side, end });

    // Each iteration adds body (1) and outside (2).
    expectValue(end, 1.f + 3.f * 3.f, "initial");

    ASSERT_TRUE(setDoubleKnob(body, "value", 4.));
    expectValue(end, 1.f + 3.f * 6.f, "edit inside the body");

    ASSERT_TRUE(setDoubleKnob(outside, "value", 0.));
    expectValue(end, 1.f + 3.f * 4.f, "edit outside, feeding the body");

    KnobDoublePtr bodyValue = std::dynamic_pointer_cast<KnobDouble>(body->getKnobByName("value"));
    ASSERT_TRUE(bool(bodyValue));
    ASSERT_NO_THROW(bodyValue->setExpression(0, side->getScriptName() + ".value.getValue(0)", false, true));
    expectValue(end, 1.f + 3.f * 5.f, "expression reading an outside knob");
    ASSERT_TRUE(setDoubleKnob(side, "value", 7.));
    expectValue(end, 1.f + 3.f * 7.f, "edit the knob the expression reads");

    // Iterations 5, 6 and 7, then 0, 1 and 2.
    ASSERT_NO_THROW(bodyValue->setExpression(0, "loop.iteration", false, true));
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamStart, 5));
    expectValue(end, 1.f + 18.f, "start 5");
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamStart, 0));
    expectValue(end, 1.f + 3.f, "start 0");
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamStep, 2));
    expectValue(end, 1.f + 6.f, "step 2");
    bodyValue->clearExpression(0, true);
    ASSERT_TRUE(setDoubleKnob(body, "value", 1.));
    expectValue(end, 1.f + 3.f, "expression cleared");

    NodePtr added = createCounting(10.);
    ASSERT_TRUE(bool(added));
    cacheEverything({ added });
    ASSERT_TRUE(added->connectInput(body, 0));
    ASSERT_TRUE(merge->replaceInput(added, 0));
    expectValue(end, 1.f + 3.f * 11.f, "node added to the body");
    ASSERT_TRUE(merge->replaceInput(body, 0));
    expectValue(end, 1.f + 3.f, "node removed from the body");

    // Accumulate sums the three iterations, each on the loop input.
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamMode, kLoopEndParamModeAccumulate));
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamOperation, kLoopEndParamOperationSum));
    expectValue(end, 3.f * 2.f, "accumulate");
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamMode, kLoopEndParamModeFeedback));
    expectValue(end, 1.f + 3.f, "feedback again");
    expectBothWays(end, countingWindow());
}

// The LoopBegin's hash moves with every edit to what the body renders, and not with edits below
// the loop.
TEST_F(NativeLoopFeedbackTest, TheLoopBeginHashFollowsTheBody)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin();
    NodePtr body = createCounting(1.);
    NodePtr merge = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    NodePtr outside = createCounting(2.);
    NodePtr side = createCounting(5.);
    NodePtr end = createFeedbackEnd(3);
    NodePtr below = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(constant && begin && body && merge && outside && side && end && below);
    ASSERT_TRUE(setLoopName(end, "fb"));
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(body->connectInput(begin, 0));
    ASSERT_TRUE(merge->connectInput(body, 0));
    ASSERT_TRUE(merge->connectInput(outside, 1));
    ASSERT_TRUE(end->connectInput(merge, 0));
    ASSERT_TRUE(below->connectInput(end, 0));

    U64 hash = begin->getContentHash();
    const auto expectMoved = [&](const char* what) {
        const U64 now = begin->getContentHash();
        EXPECT_NE(hash, now) << what;
        hash = now;
    };

    ASSERT_TRUE(setDoubleKnob(body, "value", 4.));
    expectMoved("edit inside the body");
    ASSERT_TRUE(setDoubleKnob(outside, "value", 3.));
    expectMoved("edit outside, feeding the body");
    KnobDoublePtr bodyValue = std::dynamic_pointer_cast<KnobDouble>(body->getKnobByName("value"));
    ASSERT_TRUE(bool(bodyValue));
    ASSERT_NO_THROW(bodyValue->setExpression(0, side->getScriptName() + ".value.getValue(0)", false, true));
    expectMoved("expression set");
    ASSERT_TRUE(setDoubleKnob(side, "value", 6.));
    expectMoved("edit the knob the expression reads");
    ASSERT_TRUE(setIntKnob(end, kLoopEndParamStart, 2));
    expectMoved("LoopEnd start");
    ASSERT_TRUE(setDoubleKnob(end, kLoopEndParamFrameStep, 3.));
    expectMoved("LoopEnd frame step");

    const U64 beforeAdding = hash;
    NodePtr added = createCounting(1.);
    ASSERT_TRUE(bool(added));
    ASSERT_TRUE(added->connectInput(body, 0));
    ASSERT_TRUE(merge->replaceInput(added, 0));
    expectMoved("node added to the body");
    ASSERT_TRUE(merge->replaceInput(body, 0));
    EXPECT_EQ(beforeAdding, begin->getContentHash()) << "node removed from the body";
    hash = beforeAdding;

    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamMode, kLoopEndParamModeAccumulate));
    expectMoved("mode accumulate");
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamMode, kLoopEndParamModeFeedback));
    expectMoved("mode feedback");

    KnobColorPtr multiply = std::dynamic_pointer_cast<KnobColor>(below->getKnobByName(kGradeParamMultiply));
    ASSERT_TRUE(bool(multiply));
    multiply->setValue(2., ViewSpec::all(), 0);
    EXPECT_EQ(hash, begin->getContentHash()) << "edit below the loop";
    ASSERT_TRUE(setDoubleKnob(end, kLoopEndParamFrameStep, 3.));
    EXPECT_EQ(hash, begin->getContentHash()) << "LoopEnd knob set to its value";
}

// A Group inside the body is part of it: edits to the nodes inside the Group move the LoopBegin's
// hash and reach the cached loop.
TEST_F(NativeLoopFeedbackTest, AGroupInTheBodyIsPartOfIt)
{
    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin();
    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    groupArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    groupArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    NodePtr group = getApp()->createNode(groupArgs);
    NodePtr end = createFeedbackEnd(3);
    ASSERT_TRUE(constant && begin && group && end);
    ASSERT_TRUE(setLoopName(end, "fb"));
    NodeCollectionPtr inside = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
    ASSERT_TRUE(bool(inside));
    const auto createInside = [&](const char* pluginID) {
        CreateNodeArgs args(pluginID, inside);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    };
    NodePtr groupInput = createInside(PLUGINID_NATRON_INPUT);
    NodePtr first = createInside(kTestPluginIDCounting);
    NodePtr second = createInside(kTestPluginIDCounting);
    NodePtr groupOutput = createInside(PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && first && second && groupOutput);
    ASSERT_TRUE(setDoubleKnob(first, "value", 1.));
    ASSERT_TRUE(setDoubleKnob(second, "value", 2.));
    ASSERT_TRUE(first->connectInput(groupInput, 0));
    ASSERT_TRUE(second->connectInput(first, 0));
    ASSERT_TRUE(groupOutput->connectInput(second, 0));
    ASSERT_GE(group->getNInputs(), 1);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    ASSERT_TRUE(group->connectInput(begin, 0));
    ASSERT_TRUE(end->connectInput(group, 0));
    cacheEverything({ constant, begin, first, second, end });

    // Each iteration adds first (1) and second (2).
    expectValue(end, 1.f + 3.f * 3.f, "initial");

    U64 hash = begin->getContentHash();
    const auto expectMoved = [&](const char* what) {
        const U64 now = begin->getContentHash();
        EXPECT_NE(hash, now) << what;
        hash = now;
    };

    ASSERT_TRUE(setDoubleKnob(second, "value", 4.));
    expectMoved("edit the last node in the Group");
    ASSERT_TRUE(setDoubleKnob(second, "value", 5.));
    expectMoved("edit it again");
    ASSERT_TRUE(setDoubleKnob(first, "value", 0.));
    expectMoved("edit the node before it");
    expectValue(end, 1.f + 3.f * 5.f, "after the edits");

    ASSERT_TRUE(setDoubleKnob(first, "value", 2.));
    ASSERT_TRUE(setDoubleKnob(second, "value", 1.));
    expectMoved("two edits without a render between them");
    expectValue(end, 1.f + 3.f * 3.f, "after two edits");
    expectBothWays(end, countingWindow());
}

// A feedback loop inside a feedback loop: both LoopBegins follow an edit at the core, and the
// outer loop runs the inner one in full each iteration.
TEST_F(NativeLoopFeedbackTest, NestedFeedbackLoops)
{
    NodePtr constant = createConstant(1.);
    NodePtr outerBegin = createLoopBegin();
    NodePtr innerBegin = createLoopBegin();
    NodePtr body = createCounting(1.);
    NodePtr innerEnd = createFeedbackEnd(3);
    NodePtr outerEnd = createFeedbackEnd(4);
    ASSERT_TRUE(constant && outerBegin && innerBegin && body && innerEnd && outerEnd);
    ASSERT_TRUE(setLoopName(outerEnd, "outer"));
    ASSERT_TRUE(setLoopName(innerEnd, "inner"));
    ASSERT_TRUE(outerBegin->connectInput(constant, 0));
    ASSERT_TRUE(innerBegin->connectInput(outerBegin, 0));
    ASSERT_TRUE(body->connectInput(innerBegin, 0));
    ASSERT_TRUE(innerEnd->connectInput(body, 0));
    ASSERT_TRUE(outerEnd->connectInput(innerEnd, 0));
    cacheEverything({ outerBegin, innerBegin, body, innerEnd, outerEnd });

    // The outer feedback edge puts the inner LoopEnd on a cycle, yet what it sets stays out of its
    // read-set: an outer iteration is one image whichever inner iteration asks for it.
    const RenderContextName innerT = RenderContextName::loops("inner", "t");
    const RenderContextName outerT = RenderContextName::loops("outer", "t");
    EXPECT_TRUE(body->getUpstreamContextReadSet()->contains(innerT));
    EXPECT_TRUE(body->getUpstreamContextReadSet()->contains(outerT));
    EXPECT_FALSE(innerEnd->getUpstreamContextReadSet()->contains(innerT));
    EXPECT_TRUE(innerEnd->getUpstreamContextReadSet()->contains(outerT));
    EXPECT_FALSE(outerBegin->getUpstreamContextReadSet()->contains(innerT));
    EXPECT_TRUE(outerBegin->getUpstreamContextReadSet()->contains(outerT));
    EXPECT_FALSE(outerEnd->getUpstreamContextReadSet()->contains(outerT));

    const U64 outerHash = outerBegin->getContentHash();
    const U64 innerHash = innerBegin->getContentHash();
    expectValue(outerEnd, 1.f + 12.f, "nested");
    ASSERT_TRUE(setDoubleKnob(body, "value", 2.));
    EXPECT_NE(outerHash, outerBegin->getContentHash());
    EXPECT_NE(innerHash, innerBegin->getContentHash());
    expectValue(outerEnd, 1.f + 24.f, "nested, after an edit");
    expectBothWays(outerEnd, countingWindow());
}

// A 200-node body over 1024 iterations: the scheduler stays within its bytes budget and plans
// every pull, and the legacy render does each iteration's work once.
TEST_F(NativeLoopFeedbackTest, ALongFeedbackChainStaysBounded)
{
    const int bodySize = 200;
    const int iterations = 1024;
    const std::size_t imageBytes = (std::size_t)kCountingTestSize * kCountingTestSize * 4 * sizeof(float);
    const std::size_t budget = 6 * imageBytes;

    NodePtr constant = createConstant(1.);
    NodePtr begin = createLoopBegin();
    NodePtr end = createFeedbackEnd(iterations);
    ASSERT_TRUE(constant && begin && end);
    ASSERT_TRUE(begin->connectInput(constant, 0));
    NodePtr previous = begin;
    for (int i = 0; i < bodySize; ++i) {
        NodePtr counting = createCounting(1.);
        ASSERT_TRUE(counting && counting->connectInput(previous, 0));
        previous = counting;
    }
    ASSERT_TRUE(end->connectInput(previous, 0));
    const float expected = 1.f + (float)(bodySize * iterations);

    CountingTestRegistry::reset();
    float value = -1.f;
    ASSERT_TRUE(renderedValue(end, eRenderSchedulerModeLegacy, true, &value));
    EXPECT_EQ(expected, value);
    EXPECT_EQ(bodySize * iterations, CountingTestRegistry::totalRenders());

    BytesBudgetGuard guard(budget);
    RenderScheduler* scheduler = appPTR->getRenderScheduler();
    scheduler->resetPeakBytesInFlight();
    std::vector<int> poolSizes({ 4 });
    std::vector<int> unplanned;
    const RenderMismatch m = renderBothWaysDirect(end, kTime, ViewIdx(0), 0, countingWindow(), poolSizes, std::function<void()>(), 0.f, &unplanned);
    EXPECT_FALSE(m.any) << describe(m);
    ASSERT_EQ(poolSizes.size(), unplanned.size());
    EXPECT_EQ(0, unplanned[0]);
    EXPECT_GT(scheduler->getPeakBytesInFlight(), (std::size_t)0);
    EXPECT_LE(scheduler->getPeakBytesInFlight(), budget + imageBytes) << "peak of " << ((double)scheduler->getPeakBytesInFlight() / (double)imageBytes) << " images";
}

// The loop is the LoopEnd's: both LoopBegins pair with it, and only the one with Feedback on
// outputs the iteration before. 1 + 10, then 10 more per iteration: 41.
TEST_F(NativeLoopFeedbackTest, TwoLoopBeginsShareOneLoop)
{
    SharedLoop loop;
    ASSERT_TRUE(createSharedLoop(&loop));
    LoopEnd* end = dynamic_cast<LoopEnd*>(loop.end->getEffectInstance().get());
    ASSERT_TRUE(end);
    EXPECT_EQ(std::vector<NodePtr>({ loop.first, loop.second }), end->getLoopBegins());
    EXPECT_EQ(std::vector<NodePtr>({ loop.first }), end->getFeedbackBegins());
    EXPECT_FALSE(end->getLoopName().empty());
    cacheEverything({ loop.first, loop.second, loop.sum, loop.end });

    expectValue(loop.end, 41.f, "one LoopBegin feeding back");
    expectBothWays(loop.end, countingWindow());

    // Both feed back: 11, doubled at every iteration after the first.
    const U64 firstHash = loop.first->getContentHash();
    ASSERT_TRUE(setBeginFeedsBack(loop.end, loop.second, true));
    EXPECT_EQ(std::vector<NodePtr>({ loop.first, loop.second }), end->getFeedbackBegins());
    EXPECT_NE(firstHash, loop.first->getContentHash());
    expectValue(loop.end, 88.f, "both LoopBegins feeding back");
    expectBothWays(loop.end, countingWindow());

    // Accumulate renders every iteration from both inputs: 4 * 11.
    ASSERT_TRUE(setChoiceKnob(loop.end, kLoopEndParamMode, kLoopEndParamModeAccumulate));
    ASSERT_TRUE(setChoiceKnob(loop.end, kLoopEndParamOperation, kLoopEndParamOperationSum));
    EXPECT_TRUE(end->getFeedbackBegins().empty());
    expectValue(loop.end, 44.f, "accumulated");
    expectBothWays(loop.end, countingWindow());
}

// Which LoopBegins feed back is the LoopEnd's alone: LoopBegin has no knob of its own, and a
// LoopBegin keeps its choice across a rename.
TEST_F(NativeLoopFeedbackTest, TheLoopEndChoosesWhichLoopBeginsFeedBack)
{
    SharedLoop loop;
    ASSERT_TRUE(createSharedLoop(&loop));
    LoopEnd* end = dynamic_cast<LoopEnd*>(loop.end->getEffectInstance().get());
    ASSERT_TRUE(end);
    EXPECT_FALSE(bool(loop.first->getKnobByName("feedback")));
    LoopBegin* second = dynamic_cast<LoopBegin*>(loop.second->getEffectInstance().get());
    ASSERT_TRUE(second);
    EXPECT_FALSE(second->feedsBack());
    EXPECT_EQ(std::vector<std::string>({ loop.second->getScriptName() }), end->getPassThroughBeginNames());

    ASSERT_NO_THROW(loop.second->setScriptName("RenamedSecondBegin"));
    EXPECT_EQ(std::vector<std::string>({ std::string("RenamedSecondBegin") }), end->getPassThroughBeginNames());
    EXPECT_EQ(std::vector<NodePtr>({ loop.first }), end->getFeedbackBegins());
    expectValue(loop.end, 41.f, "after renaming the LoopBegin passing through");

    const U64 firstHash = loop.first->getContentHash();
    ASSERT_TRUE(setBeginFeedsBack(loop.end, loop.second, true));
    EXPECT_TRUE(end->getPassThroughBeginNames().empty());
    EXPECT_TRUE(second->feedsBack());
    EXPECT_NE(firstHash, loop.first->getContentHash());
    expectValue(loop.end, 88.f, "both LoopBegins feeding back");
}

// The per-LoopBegin choice shows only in Feedback mode or under Cross Frame with several
// LoopBegins wired: with one, Mode and Cross Frame alone decide.
TEST_F(NativeLoopFeedbackTest, FeedbackLoopBeginsShowOnlyWithSeveralLoopBegins)
{
    NodePtr one = createConstant(1.);
    NodePtr ten = createConstant(10.);
    NodePtr first = createLoopBegin();
    NodePtr second = createLoopBegin();
    NodePtr sum = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    NodePtr end = createFeedbackEnd(4);
    ASSERT_TRUE(one && ten && first && second && sum && end);
    EXPECT_TRUE(isKnobSecret(end, kLoopEndParamPassThroughBegins)) << "no LoopBegin";

    ASSERT_TRUE(first->connectInput(one, 0));
    ASSERT_TRUE(second->connectInput(ten, 0));
    ASSERT_TRUE(sum->connectInput(first, 0));
    ASSERT_TRUE(end->connectInput(sum, 0));
    EXPECT_TRUE(isKnobSecret(end, kLoopEndParamPassThroughBegins)) << "one LoopBegin";

    // Wiring the body refreshes the LoopEnd once the event loop runs.
    ASSERT_TRUE(sum->connectInput(second, 1));
    QCoreApplication::processEvents();
    EXPECT_FALSE(isKnobSecret(end, kLoopEndParamPassThroughBegins)) << "two LoopBegins";

    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamMode, kLoopEndParamModeAccumulate));
    EXPECT_TRUE(isKnobSecret(end, kLoopEndParamPassThroughBegins)) << "Accumulate";
    ASSERT_TRUE(setBoolKnob(end, kLoopEndParamCrossFrame, true));
    EXPECT_FALSE(isKnobSecret(end, kLoopEndParamPassThroughBegins)) << "Accumulate, Cross Frame";
    ASSERT_TRUE(setBoolKnob(end, kLoopEndParamCrossFrame, false));
    EXPECT_TRUE(isKnobSecret(end, kLoopEndParamPassThroughBegins)) << "Accumulate again";
    ASSERT_TRUE(setChoiceKnob(end, kLoopEndParamMode, kLoopEndParamModeFeedback));
    EXPECT_FALSE(isKnobSecret(end, kLoopEndParamPassThroughBegins)) << "Feedback again";

    sum->disconnectInput(1);
    QCoreApplication::processEvents();
    EXPECT_TRUE(isKnobSecret(end, kLoopEndParamPassThroughBegins)) << "back to one LoopBegin";
}

// The pairing a LoopEnd and its LoopBegins keep between edits follows every rewiring and rename.
TEST_F(NativeLoopFeedbackTest, ThePairingFollowsRewiringAndRenames)
{
    SharedLoop loop;
    ASSERT_TRUE(createSharedLoop(&loop));
    LoopEnd* end = dynamic_cast<LoopEnd*>(loop.end->getEffectInstance().get());
    ASSERT_TRUE(end);
    EXPECT_EQ(std::vector<NodePtr>({ loop.first, loop.second }), end->getLoopBegins());
    EXPECT_EQ(end, LoopEnd::pairedEndOf(loop.second).get());

    loop.sum->disconnectInput(1);
    EXPECT_EQ(std::vector<NodePtr>({ loop.first }), end->getLoopBegins());
    EXPECT_FALSE(LoopEnd::pairedEndOf(loop.second)) << "disconnected";

    ASSERT_TRUE(loop.sum->connectInput(loop.second, 1));
    EXPECT_EQ(std::vector<NodePtr>({ loop.first, loop.second }), end->getLoopBegins());
    EXPECT_EQ(end, LoopEnd::pairedEndOf(loop.second).get()) << "reconnected";

    // The LoopBegins are listed by script name.
    ASSERT_NO_THROW(loop.first->setScriptName("ZLastBegin"));
    EXPECT_EQ(std::vector<NodePtr>({ loop.second, loop.first }), end->getLoopBegins());
    EXPECT_EQ(std::vector<NodePtr>({ loop.first }), end->getFeedbackBegins());
    expectValue(loop.end, 41.f, "after rewiring and renaming");
}

// Feedback LoopBegins lists LoopBegins by script name, which a LoopBegin inside a Group may share
// with one outside it: a loop opened by two LoopBegins of one name is an error naming both.
TEST_F(NativeLoopFeedbackTest, LoopBeginsOfOneLoopNeedDifferentNames)
{
    NodePtr one = createConstant(1.);
    NodePtr ten = createConstant(10.);
    NodePtr outerBegin = createLoopBegin();
    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    groupArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    groupArgs.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    NodePtr group = getApp()->createNode(groupArgs);
    NodePtr sum = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    NodePtr endNode = createFeedbackEnd(4);
    ASSERT_TRUE(one && ten && outerBegin && group && sum && endNode);
    ASSERT_TRUE(setDoubleKnob(sum, "value", 0.));
    NodeCollectionPtr inside = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
    ASSERT_TRUE(bool(inside));
    const auto createInside = [&](const char* pluginID) {
        CreateNodeArgs args(pluginID, inside);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    };
    NodePtr groupInput = createInside(PLUGINID_NATRON_INPUT);
    NodePtr innerBegin = createInside(PLUGINID_NATRON_LOOPBEGIN);
    NodePtr groupOutput = createInside(PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerBegin && groupOutput);
    ASSERT_TRUE(innerBegin->connectInput(groupInput, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerBegin, 0));
    ASSERT_GE(group->getNInputs(), 1);
    ASSERT_TRUE(outerBegin->connectInput(one, 0));
    ASSERT_TRUE(group->connectInput(ten, 0));
    ASSERT_TRUE(sum->connectInput(outerBegin, 0));
    ASSERT_TRUE(sum->connectInput(group, 1));
    ASSERT_TRUE(endNode->connectInput(sum, 0));
    LoopEnd* end = dynamic_cast<LoopEnd*>(endNode->getEffectInstance().get());
    ASSERT_TRUE(end);

    ASSERT_NO_THROW(outerBegin->setScriptName("SharedBegin"));
    ASSERT_NO_THROW(innerBegin->setScriptName("InnerBegin"));
    EXPECT_EQ(std::vector<NodePtr>({ innerBegin, outerBegin }), end->getLoopBegins());
    ASSERT_TRUE(setBeginFeedsBack(endNode, innerBegin, false));
    EXPECT_EQ(std::vector<NodePtr>({ outerBegin }), end->getFeedbackBegins());
    expectValue(endNode, 41.f, "the LoopBegin in the Group passing through");

    ASSERT_NO_THROW(innerBegin->setScriptName("SharedBegin"));
    EXPECT_TRUE(end->getLoopBegins().empty());
    EXPECT_FALSE(LoopEnd::pairedEndOf(innerBegin));
    EXPECT_FALSE(LoopEnd::pairedEndOf(outerBegin));
    const QString message = loopErrorOf(endNode);
    EXPECT_TRUE(message.contains(QString::fromUtf8("both named"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(innerBegin->getFullyQualifiedName().c_str()))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(outerBegin->getFullyQualifiedName().c_str()))) << message.toStdString();

    // Named apart again, the LoopBegin in the Group keeps passing its input through.
    ASSERT_NO_THROW(innerBegin->setScriptName("InnerBegin"));
    EXPECT_EQ(std::vector<NodePtr>({ innerBegin, outerBegin }), end->getLoopBegins());
    EXPECT_EQ(std::vector<NodePtr>({ outerBegin }), end->getFeedbackBegins());
    expectValue(endNode, 41.f, "named apart again");
}

// What a node keeps for one render, such as a Feedback chain's last image, goes with the render.
TEST_F(NativeLoopFeedbackTest, WhatARenderKeepsAliveEndsWithIt)
{
    AbortableRenderInfoPtr render = AbortableRenderInfo::create(true, 0);
    std::shared_ptr<int> kept = std::make_shared<int>(1);
    const std::weak_ptr<int> weak = kept;
    render->keepAliveWithRender(kept);
    kept.reset();
    EXPECT_FALSE(weak.expired());
    render.reset();
    EXPECT_TRUE(weak.expired());
}

// A LoopEnd just created shows only the knobs of its default settings: Accumulate over a count,
// Cross Frame off.
TEST_F(NativeLoopFeedbackTest, AFreshLoopEndHidesInactiveKnobs)
{
    NodePtr end = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));
    ASSERT_TRUE(bool(end));
    EXPECT_TRUE(isKnobSecret(end, kLoopEndParamFramesBefore));
    EXPECT_TRUE(isKnobSecret(end, kLoopEndParamFramesAfter));
    EXPECT_TRUE(isKnobSecret(end, kLoopEndParamFrameStep));
    EXPECT_TRUE(isKnobSecret(end, kLoopEndParamPassThroughBegins));
    for (const char* crossFrameKnob : { kLoopEndParamStartFrame, kLoopEndParamSubframes, kLoopEndParamCacheFile,
                                        kLoopEndParamBakeEnd, kLoopEndParamBake, kLoopEndParamLoadFromCache,
                                        kLoopEndParamClearCache }) {
        EXPECT_TRUE(isKnobSecret(end, crossFrameKnob)) << crossFrameKnob;
    }
    EXPECT_FALSE(isKnobSecret(end, kLoopEndParamIterations));
    EXPECT_FALSE(isKnobSecret(end, kLoopEndParamOperation));
    EXPECT_FALSE(isKnobSecret(end, kLoopEndParamReverseOrder));
}

TEST_F(NativeLoopFeedbackTest, ASharedLoopSavesAndReopens)
{
    SharedLoop loop;
    ASSERT_TRUE(createSharedLoop(&loop));
    ASSERT_TRUE(setLoopName(loop.end, "shared"));
    const std::string firstName = loop.first->getScriptName();
    const std::string secondName = loop.second->getScriptName();
    const std::string endName = loop.end->getScriptName();
    expectValue(loop.end, 41.f, "before saving");

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("shared-loop.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    loop = SharedLoop();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr first = project->getNodeByName(firstName);
    NodePtr second = project->getNodeByName(secondName);
    NodePtr endNode = project->getNodeByName(endName);
    ASSERT_TRUE(first && second && endNode);
    LoopEnd* end = dynamic_cast<LoopEnd*>(endNode->getEffectInstance().get());
    ASSERT_TRUE(end);
    EXPECT_EQ(std::string("shared"), end->getLoopName());
    EXPECT_EQ(std::vector<NodePtr>({ first, second }), end->getLoopBegins());
    EXPECT_EQ(std::vector<NodePtr>({ first }), end->getFeedbackBegins());
    expectValue(endNode, 41.f, "reopened");
}

// A LoopBegin reaching two LoopEnds belongs to neither loop, and one under another LoopBegin of
// its own loop would sit in that one's body: both make the loop an error naming the nodes.
TEST_F(NativeLoopFeedbackTest, InvalidSharedLoopsAreReported)
{
    SharedLoop loop;
    ASSERT_TRUE(createSharedLoop(&loop));
    NodePtr other = createFeedbackEnd(2);
    ASSERT_TRUE(other && other->connectInput(loop.second, 0));
    LoopEnd* end = dynamic_cast<LoopEnd*>(loop.end->getEffectInstance().get());
    ASSERT_TRUE(end);
    EXPECT_TRUE(end->getLoopBegins().empty());

    QString message = loopErrorOf(loop.end);
    EXPECT_TRUE(message.contains(QString::fromUtf8("belongs to more than one loop"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(loop.second->getScriptName().c_str()))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(loop.end->getScriptName().c_str()))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(other->getScriptName().c_str()))) << message.toStdString();
    message = loopErrorOf(other);
    EXPECT_TRUE(message.contains(QString::fromUtf8("belongs to more than one loop"))) << message.toStdString();

    other->disconnectInput(0);
    EXPECT_EQ(2u, end->getLoopBegins().size());
    expectValue(loop.end, 41.f, "valid again");

    // The second LoopBegin fed from the first.
    ASSERT_TRUE(loop.second->replaceInput(loop.first, 0));
    EXPECT_TRUE(end->getLoopBegins().empty());
    message = loopErrorOf(loop.end);
    EXPECT_TRUE(message.contains(QString::fromUtf8("is inside the body"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(loop.first->getScriptName().c_str()))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8(loop.second->getScriptName().c_str()))) << message.toStdString();
}
