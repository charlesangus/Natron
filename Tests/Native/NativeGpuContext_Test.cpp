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
#include "CountingTestEffect.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobDriver.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/RFV/ReverseFlowVariableSet.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// A 16x16 RGBA pattern, so every pixel has its own value to grade.
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
expectMatchesCpu(const EndToEndResult& result)
{
    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    bool nonZero = false;
    for (float v : result.cpu) {
        nonZero = nonZero || (v != 0.f);
    }
    EXPECT_TRUE(nonZero) << "the CPU render is black, which proves nothing";
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ": CPU " << result.cpu[diff.firstMismatchIndex] << ", GPU "
                                   << result.gpu[diff.firstMismatchIndex] << "; worst " << diff.worstAbsolute;
}

} // namespace

class NativeGpuContextTest
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

    // A Grade whose multiply, in every channel, goes from start to end as variable goes from 0 to 1. A start other
    // than 1 keeps every context's task from being an identity, which would never reach the GPU.
    NodePtr createDrivenGrade(const std::string& variable,
                              double start,
                              double end)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        KnobColorPtr multiply = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kGradeParamMultiply)) : KnobColorPtr();

        EXPECT_TRUE(bool(multiply));
        if (!multiply) {
            return NodePtr();
        }
        for (int i = 0; i < multiply->getDimension(); ++i) {
            EXPECT_TRUE(multiply->setDriver(i, std::make_shared<const KnobDriver>(variable, start, end, KnobDriver::eEasingLinear)));
        }

        return node;
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

    // A Set node declaring the Double variable name at value.
    NodePtr createSet(const std::string& name,
                      double value)
    {
        NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
        if (!set) {
            ADD_FAILURE() << "no Set node";

            return NodePtr();
        }
        EffectInstancePtr effect = set->getEffectInstance();
        KnobGroupPtr variables = std::dynamic_pointer_cast<KnobGroup>(set->getKnobByName(kReverseFlowVariableSetParamVariables));
        KnobDoublePtr knob = effect->createDoubleKnob(name, name, 1);
        if (!variables || !variables->addKnob(knob)) {
            ADD_FAILURE() << "the Set node refused " << name;

            return NodePtr();
        }
        effect->recreateUserKnobs(false);
        knob->setValue(value);

        return set;
    }
};

// Each of the five iterations renders the Grade under its own loop.t, so each is a task under its own context: all of
// them go to the GPU, and the fold of what they render matches the CPU's.
TEST_F(NativeGpuContextTest, DrivenGradeInAnAccumulateLoopRendersEveryIterationOnTheGpu)
{
    NodePtr read = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/") + kPatternFixture);
    NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
    NodePtr grade = createDrivenGrade(kKnobDriverDefaultVariable, 1.5, 2.5);
    NodePtr end = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));
    ASSERT_TRUE(read && begin && grade && end);
    KnobIntPtr iterations = std::dynamic_pointer_cast<KnobInt>(end->getKnobByName(kLoopEndParamIterations));
    KnobChoicePtr mode = std::dynamic_pointer_cast<KnobChoice>(end->getKnobByName(kLoopEndParamMode));
    KnobChoicePtr operation = std::dynamic_pointer_cast<KnobChoice>(end->getKnobByName(kLoopEndParamOperation));
    ASSERT_TRUE(iterations && mode && operation);
    iterations->setValue(5);
    mode->setValueFromID(kLoopEndParamModeAccumulate, 0);
    operation->setValueFromID(kLoopEndParamOperationMean, 0);
    connectNodes(read, begin, 0, true);
    connectNodes(begin, grade, 0, true);
    connectNodes(grade, end, 0, true);

    const RectI window = windowOf(read);
    ASSERT_FALSE(window.isNull());
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ read, begin, grade, end }), end, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.ranInRegion(grade));
    EXPECT_EQ(5, gpuRunsOf(result, grade)) << "an iteration of the Grade fell back to the CPU";
    expectMatchesCpu(result);
}

// One Grade under two Set contexts renders twice on the GPU, each under its own value: had the two shared an image
// key, one would hold the other's gain and the sum would be off.
TEST_F(NativeGpuContextTest, EachSetContextRendersItsOwnImageOnTheGpu)
{
    NodePtr constant = createConstant(1.);
    NodePtr grade = createDrivenGrade("v", 1., 2.);
    NodePtr set1 = createSet("v", 0.25);
    NodePtr set2 = createSet("v", 0.75);
    NodePtr merge = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    ASSERT_TRUE(constant && grade && set1 && set2 && merge);
    connectNodes(constant, grade, 0, true);
    connectNodes(grade, set1, 0, true);
    connectNodes(grade, set2, 0, true);
    connectNodes(set1, merge, 0, true);
    connectNodes(set2, merge, 1, true);

    const RectI window(0, 0, kCountingTestSize, kCountingTestSize);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ constant, grade, set1, set2, merge }), merge, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(2, gpuRunsOf(result, grade)) << "a Set context's Grade fell back to the CPU or was shared";

    // The merge's value 1 plus the Grade's red under v = 0.25 (1.25) and under v = 0.75 (1.75).
    ASSERT_FALSE(result.cpu.empty());
    EXPECT_NEAR(1. + 1.25 + 1.75, result.cpu.front(), 1e-5);
    ASSERT_FALSE(result.gpu.empty());
    EXPECT_NEAR(1. + 1.25 + 1.75, result.gpu.front(), 1e-5);
    expectMatchesCpu(result);
}
