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

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobDriver.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorMathNode.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"
#endif

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// A 16x16 RGBA pattern, so every pixel folds its own values and alphas.
const char* const kPatternFixture = "flat-rgba-pattern.exr";

int
gpuRunsOf(const EndToEndResult& result,
          const NodePtr& node)
{
    const std::map<std::string, int>::const_iterator found = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());

    return (found == result.gpuTasksPerNode.end()) ? 0 : found->second;
}

RectI
windowOf(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    const StatusEnum st = effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);

    if ((st == eStatusFailed) || rod.isNull()) {
        return RectI();
    }

    return rod.toPixelEnclosing(0, 1.);
}

void
expectMatchesCpu(const EndToEndResult& result,
                 const ParityTolerance& tolerance,
                 const std::string& what)
{
    ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
    bool nonZero = false;
    for (float v : result.cpu) {
        nonZero = nonZero || (v != 0.f);
    }
    EXPECT_TRUE(nonZero) << what << ": the CPU render is black, which proves nothing";
    const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
    EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << ": CPU " << result.cpu[diff.firstMismatchIndex]
                                   << ", GPU " << result.gpu[diff.firstMismatchIndex] << "; worst " << diff.worstAbsolute;
}

// The regions of the GPU frame that hold `node`.
int
regionsHolding(const EndToEndResult& result,
               const NodePtr& node,
               std::vector<NodePtr>* members)
{
    int found = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        for (const NodePtr& n : nodes) {
            if (n == node) {
                ++found;
                if (members) {
                    *members = nodes;
                }
                break;
            }
        }
    }

    return found;
}

bool
holds(const std::vector<NodePtr>& members,
      const NodePtr& node)
{
    for (const NodePtr& n : members) {
        if (n == node) {
            return true;
        }
    }

    return false;
}

} // namespace

class NativeLoopFoldGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        _savedMode = appPTR->getRenderSchedulerMode();
        // LoopEnd folds as tasks, and so renders on the GPU, only under the task graph.
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeTaskGraph);
    }

    virtual void TearDown() OVERRIDE
    {
        appPTR->setRenderSchedulerMode(_savedMode);
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A Multiply whose value, in every channel, goes from start to end over the loop, so each iteration is its own
    // image. Neither end is 1, which would make an iteration an identity that never reaches the GPU.
    NodePtr createDrivenMultiply(double start,
                                 double end)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_MULTIPLY));
        KnobColorPtr value = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kColorMathParamValue)) : KnobColorPtr();

        EXPECT_TRUE(bool(value));
        if (!value) {
            return NodePtr();
        }
        for (int i = 0; i < value->getDimension(); ++i) {
            EXPECT_TRUE(value->setDriver(i, std::make_shared<const KnobDriver>(kKnobDriverDefaultVariable, start, end, KnobDriver::eEasingLinear)));
        }

        return node;
    }

    NodePtr createMultiply(double factor)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_MULTIPLY));
        KnobColorPtr value = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kColorMathParamValue)) : KnobColorPtr();

        EXPECT_TRUE(bool(value));
        if (!value) {
            return NodePtr();
        }
        for (int i = 0; i < value->getDimension(); ++i) {
            value->setValue(factor, ViewSpec::all(), i);
        }

        return node;
    }

    // A Constant of `value` in every channel over (0, 0) - (width, height).
    NodePtr createConstant(double value,
                           int width,
                           int height)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        KnobColorPtr color = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kConstantParamColor)) : KnobColorPtr();

        EXPECT_TRUE(bool(color));
        if (!color) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
        EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamBottomLeft, { 0., 0. }));
        EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { (double)width, (double)height }));
        for (int i = 0; i < color->getDimension(); ++i) {
            color->setValue(value, ViewSpec::all(), i);
        }

        return node;
    }

    // An Accumulate LoopEnd of `iterations` folding with `operation`.
    NodePtr createAccumulateEnd(int iterations,
                                const char* operation,
                                bool reverse)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));
        KnobIntPtr iterationsKnob = node ? std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kLoopEndParamIterations)) : KnobIntPtr();
        KnobChoicePtr mode = node ? std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kLoopEndParamMode)) : KnobChoicePtr();
        KnobChoicePtr operationKnob = node ? std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kLoopEndParamOperation)) : KnobChoicePtr();
        KnobBoolPtr reverseKnob = node ? std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kLoopEndParamReverseOrder)) : KnobBoolPtr();

        EXPECT_TRUE(iterationsKnob && mode && operationKnob && reverseKnob);
        if (!iterationsKnob || !mode || !operationKnob || !reverseKnob) {
            return NodePtr();
        }
        iterationsKnob->setValue(iterations);
        mode->setValueFromID(kLoopEndParamModeAccumulate, 0);
        operationKnob->setValueFromID(operation, 0);
        reverseKnob->setValue(reverse);

        return node;
    }

    NodePtr createFeedbackEnd(int iterations)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));
        KnobIntPtr iterationsKnob = node ? std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kLoopEndParamIterations)) : KnobIntPtr();
        KnobChoicePtr mode = node ? std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kLoopEndParamMode)) : KnobChoicePtr();

        EXPECT_TRUE(iterationsKnob && mode);
        if (!iterationsKnob || !mode) {
            return NodePtr();
        }
        iterationsKnob->setValue(iterations);
        mode->setValueFromID(kLoopEndParamModeFeedback, 0);

        return node;
    }

private:
    RenderSchedulerModeEnum _savedMode = eRenderSchedulerModeTaskGraph;
};

// Every operation, in both orders, folds the five iterations of a pattern on the GPU as the CPU does. Min and max
// only compare, and the body (an upload and one multiply per iteration) is exact, so they match bit for bit; mean's
// division and the other operations' rounding on the device may differ by an ulp.
TEST_F(NativeLoopFoldGpuTest, EveryOperationFoldsOnTheGpuAsOnTheCpu)
{
    struct Case {
        const char* operation;
        ParityTolerance tolerance;
    };
    const Case cases[] = {
        { kLoopEndParamOperationMin, ParityTolerance::exact() },
        { kLoopEndParamOperationMax, ParityTolerance::exact() },
        { kLoopEndParamOperationMean, ParityTolerance::arithmetic() },
        { kLoopEndParamOperationSum, ParityTolerance::arithmetic() },
        { kLoopEndParamOperationOver, ParityTolerance::arithmetic() },
        { kLoopEndParamOperationUnder, ParityTolerance::arithmetic() },
    };
    const int kIterations = 5;

    for (const Case& c : cases) {
        for (bool reverse : { false, true }) {
            const std::string what = std::string(c.operation) + (reverse ? ", reversed" : "");
            getApp()->getProject()->reset(false, true);
            NodePtr read = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/") + kPatternFixture);
            NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
            NodePtr multiply = createDrivenMultiply(0.5, 1.7);
            NodePtr end = createAccumulateEnd(kIterations, c.operation, reverse);
            ASSERT_TRUE(read && begin && multiply && end) << what;
            connectNodes(read, begin, 0, true);
            connectNodes(begin, multiply, 0, true);
            connectNodes(multiply, end, 0, true);

            const RectI window = windowOf(read);
            ASSERT_FALSE(window.isNull()) << what;
            // The Read stays on the CPU, so the body's source is an exact upload.
            const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ begin, multiply, end }), end, window, kTime);
            if (result.skipped) {
                return;
            }
            ASSERT_TRUE(result.ok) << what << ": " << result.error;
            EXPECT_EQ(kIterations, gpuRunsOf(result, end)) << what << ": a fold step fell back to the CPU";
            expectMatchesCpu(result, c.tolerance, what);
        }
    }
}

// The fold chain runs as one region with the body: no step goes back to the host, the only transfer being the
// result's download.
TEST_F(NativeLoopFoldGpuTest, FoldChainStaysOnTheDeviceInOneRegion)
{
    const int kIterations = 8;
    NodePtr constant = createConstant(0.25, 64, 64);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr multiply = createDrivenMultiply(0.5, 1.7);
    NodePtr end = createAccumulateEnd(kIterations, kLoopEndParamOperationOver, false);
    ASSERT_TRUE(constant && begin && multiply && end);
    connectNodes(constant, begin, 0, true);
    connectNodes(begin, multiply, 0, true);
    connectNodes(multiply, end, 0, true);

    const RectI window(0, 0, 64, 64);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ constant, begin, multiply, end }), end, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    std::vector<NodePtr> members;
    ASSERT_EQ(1, regionsHolding(result, end, &members)) << "the fold chain was split over regions";
    EXPECT_TRUE(holds(members, multiply));
    EXPECT_EQ(kIterations, gpuRunsOf(result, end));
    EXPECT_EQ(kIterations, gpuRunsOf(result, multiply));
    EXPECT_EQ(0, result.uploads);
    EXPECT_LE(result.downloads, 1);
    expectMatchesCpu(result, ParityTolerance::arithmetic(), "over");
}

// A 10-step feedback chain, each LoopBegin after the first forwarding the previous step's tail, runs as one region:
// the tail never leaves the device between steps.
TEST_F(NativeLoopFoldGpuTest, FeedbackChainStaysOnTheDeviceInOneRegion)
{
    const int kIterations = 10;
    NodePtr constant = createConstant(0.75, 64, 64);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr multiply = createMultiply(0.9);
    NodePtr end = createFeedbackEnd(kIterations);
    ASSERT_TRUE(constant && begin && multiply && end);
    connectNodes(constant, begin, 0, true);
    connectNodes(begin, multiply, 0, true);
    connectNodes(multiply, end, 0, true);

    const RectI window(0, 0, 64, 64);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ constant, begin, multiply, end }), end, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    std::vector<NodePtr> members;
    ASSERT_EQ(1, regionsHolding(result, end, &members)) << "the feedback chain was split over regions";
    EXPECT_TRUE(holds(members, multiply));
    EXPECT_TRUE(holds(members, begin));
    EXPECT_EQ(kIterations, gpuRunsOf(result, multiply));
    // The first iteration's LoopBegin is its input; every later one forwards the tail.
    EXPECT_EQ(kIterations - 1, gpuRunsOf(result, begin));
    EXPECT_EQ(0, result.uploads);
    EXPECT_LE(result.downloads, 1);
    expectMatchesCpu(result, ParityTolerance::exact(), "feedback");
}

#ifdef NATRON_HAVE_GPU

// 256 iterations of a 4K image sum under a pool budget of two images, less than one whole-image step needs (the
// step before, the iteration and the result): strips and buffer reuse keep the pool within it, and the sum is the
// CPU's.
TEST_F(NativeLoopFoldGpuTest, ManyIterationsAt4KStayWithinThePoolBudget)
{
    const int kIterations = 256;
    const int kWidth = 3840;
    const int kHeight = 2160;
    const unsigned long long image = (unsigned long long)kWidth * kHeight * 4 * sizeof(float);
    const unsigned long long budget = 2 * image;
    NodePtr constant = createConstant(0.25, kWidth, kHeight);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr end = createAccumulateEnd(kIterations, kLoopEndParamOperationSum, false);
    ASSERT_TRUE(constant && begin && end);
    connectNodes(constant, begin, 0, true);
    connectNodes(begin, end, 0, true);

    const RectI window(0, 0, kWidth, kHeight);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ constant, begin, end }), end, window, kTime, [budget](std::string*) {
        appPTR->getGpuContext()->runtime()->pool().setUserBudget(budget);

        return true;
    });
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(kIterations, gpuRunsOf(result, end)) << "a fold step fell back to the CPU";
    EXPECT_GT(result.poolPeakBytes, 0ull);
    EXPECT_LE(result.poolPeakBytes, budget);
    ASSERT_FALSE(result.gpu.empty());
    // 256 quarters sum exactly.
    EXPECT_EQ(64.f, result.gpu.front());
    expectMatchesCpu(result, ParityTolerance::arithmetic(), "sum at 4K");
}

// A pool budget below one image leaves no step room on the device: the fold falls back to the CPU and the render
// still succeeds with the CPU's result.
TEST_F(NativeLoopFoldGpuTest, BudgetBelowOneImageFallsBackToTheCpu)
{
    const int kIterations = 4;
    NodePtr constant = createConstant(0.25, 64, 64);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr multiply = createDrivenMultiply(0.5, 1.7);
    NodePtr end = createAccumulateEnd(kIterations, kLoopEndParamOperationMean, false);
    ASSERT_TRUE(constant && begin && multiply && end);
    connectNodes(constant, begin, 0, true);
    connectNodes(begin, multiply, 0, true);
    connectNodes(multiply, end, 0, true);

    const RectI window(0, 0, 64, 64);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ constant, begin, multiply, end }), end, window, kTime, [](std::string*) {
        appPTR->getGpuContext()->runtime()->pool().setUserBudget(1);

        return true;
    });
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(0, gpuRunsOf(result, end));
    const std::map<std::string, int>::const_iterator outOfMemory = result.cpuFallbackReasons.find("out of device memory");
    EXPECT_TRUE(outOfMemory != result.cpuFallbackReasons.end()) << "the fold did not fall back for want of device memory";
    expectMatchesCpu(result, ParityTolerance::exact(), "fallback");
}

#endif // NATRON_HAVE_GPU
