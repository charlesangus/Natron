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
#include "NativeShapeTestSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobDriver.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/Ramp.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/Loop/LoopMapBlend.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

using namespace LoopMapBlend;

namespace {

const double kTime = 1.;
const int kWidth = 256;
const int kHeight = 64;
const int kHalf = kWidth / 2;

RectI
fullWindow()
{
    return RectI(0, 0, kWidth, kHeight);
}

int
gpuRunsOf(const EndToEndResult& result,
          const NodePtr& node)
{
    const std::map<std::string, int>::const_iterator found = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());

    return (found == result.gpuTasksPerNode.end()) ? 0 : found->second;
}

void
expectMatchesCpu(const EndToEndResult& result,
                 const std::string& what)
{
    ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
    bool nonZero = false;
    for (float v : result.cpu) {
        nonZero = nonZero || (v != 0.f);
    }
    EXPECT_TRUE(nonZero) << what << ": the CPU render is black, which proves nothing";
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::arithmetic());
    EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << ": CPU " << result.cpu[diff.firstMismatchIndex]
                                   << ", GPU " << result.gpu[diff.firstMismatchIndex] << "; worst " << diff.worstAbsolute;
}

// Enables LoopMapBlend::TileStats for the scope.
class TileStatsGuard {
public:
    TileStatsGuard()
    {
        TileStats::setEnabled(true);
    }

    ~TileStatsGuard()
    {
        TileStats::setEnabled(false);
    }

    TileStatsGuard(const TileStatsGuard&) = delete;
    TileStatsGuard& operator=(const TileStatsGuard&) = delete;
};

} // namespace

class NativeLoopMapBlendGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        NativeShapeTest::resetProject(getApp(), kWidth, kHeight);
        _savedMode = appPTR->getRenderSchedulerMode();
        // Render continuations reach the GPU only under the task graph.
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeTaskGraph);
    }

    virtual void TearDown() OVERRIDE
    {
        TileStats::setEnabled(false);
        appPTR->setRenderSchedulerMode(_savedMode);
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A Ramp from 0 at p0 to 1 at p1 over extent.
    NodePtr createRamp(const RectI& extent,
                       double p0x,
                       double p0y,
                       double p1x,
                       double p1y)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAMP));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
        EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamBottomLeft, { (double)extent.x1, (double)extent.y1 }));
        EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { (double)extent.width(), (double)extent.height() }));
        EXPECT_TRUE(setKnobValues(node, kRampParamPoint0, { p0x, p0y }));
        EXPECT_TRUE(setKnobValues(node, kRampParamPoint1, { p1x, p1y }));

        return node;
    }

    // A Grade whose multiply, in every channel, goes from start to end over loop.t. Neither end is 1,
    // which would make an iteration an identity.
    NodePtr createDrivenGrade(double start,
                              double end)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        KnobColorPtr multiply = node ? std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kGradeParamMultiply)) : KnobColorPtr();

        EXPECT_TRUE(bool(multiply));
        if (!multiply) {
            return NodePtr();
        }
        for (int i = 0; i < multiply->getDimension(); ++i) {
            EXPECT_TRUE(multiply->setDriver(i, std::make_shared<const KnobDriver>(kKnobDriverDefaultVariable, start, end, KnobDriver::eEasingLinear)));
        }

        return node;
    }

    NodePtr createMapBlendEnd(int iterations,
                              const std::string& interpolation)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));
        KnobIntPtr iterationsKnob = node ? std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kLoopEndParamIterations)) : KnobIntPtr();
        KnobChoicePtr mode = node ? std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kLoopEndParamMode)) : KnobChoicePtr();
        KnobChoicePtr interpolationKnob = node ? std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kLoopEndParamInterpolation)) : KnobChoicePtr();

        EXPECT_TRUE(iterationsKnob && mode && interpolationKnob);
        if (!iterationsKnob || !mode || !interpolationKnob) {
            return NodePtr();
        }
        iterationsKnob->setValue(iterations);
        mode->setValueFromID(kLoopEndParamModeMapBlend, 0);
        interpolationKnob->setValueFromID(interpolation, 0);
        EXPECT_EQ(interpolation, interpolationKnob->getActiveEntry().id);

        return node;
    }

    // A horizontal ramp -> LoopBegin -> Grade driven 0.25 to 1.75 -> a Map Blend end of iterations,
    // with map on its Map input when there is one.
    NodePtr createLoop(int iterations,
                       const std::string& interpolation,
                       const NodePtr& map)
    {
        NodePtr source = createRamp(fullWindow(), 0., 0., (double)kWidth, 0.);
        NodePtr begin = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
        NodePtr grade = createDrivenGrade(0.25, 1.75);
        NodePtr end = createMapBlendEnd(iterations, interpolation);

        EXPECT_TRUE(source && begin && grade && end);
        if (!source || !begin || !grade || !end) {
            return NodePtr();
        }
        connectNodes(source, begin, 0, true);
        connectNodes(begin, grade, 0, true);
        connectNodes(grade, end, kLoopEndInputSource, true);
        if (map) {
            connectNodes(map, end, kLoopEndInputMap, true);
        }

        return end;
    }

    void expectBlendsOnTheGpu(const EndToEndResult& result,
                              const NodePtr& end,
                              const std::string& what)
    {
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(end)) << what;
        EXPECT_EQ(1, gpuRunsOf(result, end)) << what << ": the blend fell back to the CPU";
        expectMatchesCpu(result, what);
    }

private:
    RenderSchedulerModeEnum _savedMode = eRenderSchedulerModeTaskGraph;
};

// A vertical ramp Map spans every level, most pixels between two iterations: each interpolation
// blends on the GPU as on the CPU. The Map is offered to placement too, so a Map the GPU renders
// is downloaded for the probe and read on the device by the blend.
TEST_F(NativeLoopMapBlendGpuTest, EveryInterpolationBlendsAsOnTheCpu)
{
    const char* const interpolations[] = {
        kLoopEndParamInterpolationLinear,
        kLoopEndParamInterpolationCubic,
        kLoopEndParamInterpolationNearest,
    };

    for (const char* interpolation : interpolations) {
        getApp()->getProject()->reset(false, true);
        NativeShapeTest::resetProject(getApp(), kWidth, kHeight);
        NodePtr map = createRamp(fullWindow(), 0., 0., 0., (double)kHeight);
        NodePtr end = createLoop(5, interpolation, map);
        ASSERT_TRUE(map && end) << interpolation;

        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ map, end }), end, fullWindow(), kTime);
        if (result.skipped) {
            return;
        }
        expectBlendsOnTheGpu(result, end, interpolation);
    }
}

// A Map black on its left half and a ramp on its right: the probe of the GPU frame asks for one
// iteration over the left tiles and for all of them over the right ones, and the blend still
// matches the CPU.
TEST_F(NativeLoopMapBlendGpuTest, HalfBlackMapCullsIterationsPerTile)
{
    const int count = 5;
    NodePtr map = createRamp(RectI(kHalf, 0, kWidth, kHeight), (double)kHalf, 0., (double)kHalf, (double)kHeight);
    NodePtr end = createLoop(count, kLoopEndParamInterpolationLinear, map);
    ASSERT_TRUE(map && end);

    TileStatsGuard guard;
    (void)TileStats::take();
    std::vector<TileStats::Entry> entries;
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ end }), end, fullWindow(), kTime, GpuContextReady(), [&entries]() {
        entries = TileStats::take();
    });
    if (result.skipped) {
        return;
    }
    expectBlendsOnTheGpu(result, end, "half-black Map");

    int left = 0, right = 0;
    for (const TileStats::Entry& entry : entries) {
        if (entry.tile.x2 <= kHalf) {
            ++left;
            EXPECT_EQ(1, entry.iterations) << "left tile at " << entry.tile.x1 << "," << entry.tile.y1;
        } else {
            ASSERT_GE(entry.tile.x1, kHalf);
            ++right;
            EXPECT_EQ(count, entry.iterations) << "right tile at " << entry.tile.x1 << "," << entry.tile.y1;
        }
    }
    EXPECT_GT(left, 0);
    EXPECT_GT(right, 0);
}

// Without a Map every pixel reads 0, the first iteration, on the GPU as on the CPU.
TEST_F(NativeLoopMapBlendGpuTest, MissingMapReadsTheFirstIteration)
{
    NodePtr end = createLoop(5, kLoopEndParamInterpolationLinear, NodePtr());
    ASSERT_TRUE(end != nullptr);

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ end }), end, fullWindow(), kTime);
    if (result.skipped) {
        return;
    }
    expectBlendsOnTheGpu(result, end, "missing Map");
}
