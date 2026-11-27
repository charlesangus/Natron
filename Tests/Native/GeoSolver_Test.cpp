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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "NativePluginIdentity.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobDriver.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoTransform.h"
#include "Engine/Nodes/Geometry/GeoWrangle.h"
#include "Engine/Nodes/Loop/GeoForEachBegin.h"
#include "Engine/Nodes/Loop/GeoForEachEnd.h"
#include "Engine/Nodes/Loop/GeoSimCache.h"
#include "Engine/Nodes/Loop/GeoSolverBegin.h"
#include "Engine/Nodes/Loop/GeoSolverEnd.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderContext.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

std::vector<float>
positions(const GeoDetail& detail)
{
    const GeoAttribute& p = detail.position();
    std::vector<float> out(p.size() * 3);

    p.copyDense<float>(0, p.size(), std::span<float>(out));

    return out;
}

// The mean y of the points [first, first + count) of p, every point by default.
double
meanY(const std::vector<float>& p,
      std::size_t first = 0,
      std::size_t count = (std::size_t)-1)
{
    const std::size_t n = std::min(count, p.size() / 3 - first);
    double sum = 0.;

    for (std::size_t i = first; i < first + n; ++i) {
        sum += p[3 * i + 1];
    }

    return n ? sum / (double)n : 0.;
}

} // namespace

class GeoSolverTest
    : public BaseTest {
protected:
    DisableUnreachableRAMPurging _noPurging;

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _cube = add(PLUGINID_NATRON_GEOPRIMITIVE);
        ASSERT_TRUE(_cube != NULL);
        KnobChoicePtr type = std::dynamic_pointer_cast<KnobChoice>(_cube->getKnobByName(kGeoPrimitiveParamType));
        ASSERT_TRUE(type != NULL);
        type->setValue((int)eGeoShapeCube);
    }

    virtual void TearDown() OVERRIDE
    {
        for (auto it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
            appPTR->removeAllCacheEntriesForHolder(it->get(), true);
            (*it)->destroyNode(false, false);
        }
        _nodes.clear();
        _cube.reset();
        BaseTest::TearDown();
    }

    NodePtr add(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(node != NULL) << pluginID;
        if (node) {
            _nodes.push_back(node);
        }

        return node;
    }

    NodePtr begin(const NodePtr& input,
                  GeoSolverBeginMethodEnum method = eGeoSolverBeginPrevFrame)
    {
        NodePtr node = add(PLUGINID_NATRON_GEOSOLVERBEGIN);

        if (node) {
            if (input) {
                connectNodes(input, node, 0, true);
            }
            setChoice(node, kGeoSolverBeginParamMethod, (int)method);
        }

        return node;
    }

    NodePtr end(const NodePtr& body)
    {
        NodePtr node = add(PLUGINID_NATRON_GEOSOLVEREND);

        if (node) {
            connectNodes(body, node, 0, true);
        }

        return node;
    }

    NodePtr transform(const NodePtr& input)
    {
        NodePtr node = add(PLUGINID_NATRON_GEOTRANSFORM);

        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    NodePtr wrangle(const NodePtr& input,
                    const std::string& snippet)
    {
        NodePtr node = add(PLUGINID_NATRON_GEOWRANGLE);

        if (!node) {
            return node;
        }
        if (input) {
            connectNodes(input, node, 0, true);
        }
        setString(node, kGeoWrangleParamSnippet, snippet);
        setChoice(node, kGeoWrangleParamRunOver, 0);

        return node;
    }

    // _cube -> GeoSolverBegin -> a GeoTransform moving y by step -> GeoSolverEnd.
    NodePtr risingSolver(double step,
                         NodePtr* move = NULL)
    {
        NodePtr previous = begin(_cube);
        NodePtr body = transform(previous);
        setDouble(body, kGeoTransformParamTranslate, step, 1);
        if (move) {
            *move = body;
        }

        return end(body);
    }

    static std::shared_ptr<GeoSolverEnd> solver(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<GeoSolverEnd>(node->getEffectInstance());
    }

    static void setString(const NodePtr& node,
                          const char* name,
                          const std::string& value)
    {
        KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    static void setChoice(const NodePtr& node,
                          const char* name,
                          int value)
    {
        KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    static void setInt(const NodePtr& node,
                       const char* name,
                       int value)
    {
        KnobIntPtr knob = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    static void setBool(const NodePtr& node,
                        const char* name,
                        bool value)
    {
        KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    static void setDouble(const NodePtr& node,
                          const char* name,
                          double value,
                          int dimension)
    {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value, ViewSpec::all(), dimension);
    }

    static void drive(const NodePtr& node,
                      const char* name,
                      int dimension,
                      const std::string& variable,
                      double start,
                      double end)
    {
        std::shared_ptr<KnobHelper> knob = std::dynamic_pointer_cast<KnobHelper>(node->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        ASSERT_TRUE(knob->setDriver(dimension, std::make_shared<const KnobDriver>(variable, start, end, KnobDriver::eEasingLinear)));
    }

    EffectInstance::RenderRoIRetCode pull(const NodePtr& node,
                                          double time,
                                          GeoDetailConstPtr* detail,
                                          std::string* error,
                                          bool byPassCache = true,
                                          AbortableRenderInfoPtr abortInfo = AbortableRenderInfoPtr())
    {
        if (!abortInfo) {
            abortInfo = AbortableRenderInfo::create(true, 0);
        }
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

        return node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0), byPassCache), detail, error);
    }

    // The mean y of node's output at time, less the cube's.
    double rise(const NodePtr& node,
                double time,
                bool byPassCache = true)
    {
        GeoDetailConstPtr result;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(node, time, &result, &error, byPassCache)) << error;
        EXPECT_TRUE(result != NULL);
        if (!result) {
            return 0.;
        }
        EXPECT_EQ(cube().size(), positions(*result).size());

        return meanY(positions(*result)) - meanY(cube());
    }

    const std::vector<float>& cube()
    {
        if (_cubePositions.empty()) {
            GeoDetailConstPtr source;
            std::string error;
            EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_cube, 1., &source, &error)) << error;
            if (source) {
                _cubePositions = positions(*source);
            }
        }

        return _cubePositions;
    }

    U64 contentHash(const NodePtr& node,
                    double time)
    {
        GeoDetailConstPtr result;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(node, time, &result, &error)) << error;

        return result ? result->contentHash() : 0;
    }

    NodePtr _cube;
    std::vector<NodePtr> _nodes;
    std::vector<float> _cubePositions;
};

TEST_F(GeoSolverTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_GEOSOLVERBEGIN, 1, "GeoSolverBegin");
    expectNativeIdentity(PLUGINID_NATRON_GEOSOLVEREND, 1, "GeoSolverEnd");
}

TEST_F(GeoSolverTest, EachFrameAppliesTheBodyOnceToTheFrameBefore)
{
    NodePtr sim = risingSolver(1.);

    ASSERT_FALSE(cube().empty());
    EXPECT_NEAR(0., rise(sim, 1.), 1e-4);
    EXPECT_NEAR(4., rise(sim, 5.), 1e-4);
    EXPECT_NEAR(11., rise(sim, 12.), 1e-4);
    EXPECT_NEAR(2., rise(sim, 3.), 1e-4);
}

TEST_F(GeoSolverTest, AStartFrameLaterThanOneShiftsTheSimulation)
{
    NodePtr sim = risingSolver(1.);

    setInt(sim, kGeoSolverEndParamStartFrame, 10);
    EXPECT_NEAR(0., rise(sim, 10.), 1e-4);
    EXPECT_NEAR(5., rise(sim, 15.), 1e-4);
}

TEST_F(GeoSolverTest, FramesBeforeTheStartFrameAreTheInput)
{
    NodePtr sim = risingSolver(1.);

    setInt(sim, kGeoSolverEndParamStartFrame, 10);
    EXPECT_NEAR(0., rise(sim, 3.), 1e-4);
    EXPECT_NEAR(0., rise(sim, -20.), 1e-4);
    EXPECT_EQ(0u, solver(sim)->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, ATimeBetweenFramesShowsTheFrameBefore)
{
    NodePtr sim = risingSolver(1.);

    EXPECT_NEAR(4., rise(sim, 5.75), 1e-4);
}

// A time-invariant input still gives a different result at each frame, through the geometry cache too.
TEST_F(GeoSolverTest, TheOutputVariesWithTheFrameThroughTheCache)
{
    NodePtr sim = risingSolver(1.);

    EXPECT_NEAR(2., rise(sim, 3., false), 1e-4);
    EXPECT_NEAR(4., rise(sim, 5., false), 1e-4);
    EXPECT_NEAR(2., rise(sim, 3., false), 1e-4);
}

TEST_F(GeoSolverTest, FourSubstepsOfAQuarterFrameGiveTheSameResult)
{
    NodePtr previous = begin(_cube);
    NodePtr move = transform(previous);
    drive(move, kGeoTransformParamTranslate, 1, "loop.dt", 0., 1.);
    NodePtr sim = end(move);
    setInt(sim, kGeoSolverEndParamSubsteps, 4);

    EXPECT_NEAR(4., rise(sim, 5.), 1e-4);
    EXPECT_EQ(4u, solver(sim)->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, TimeIncIsOneSubstepInSeconds)
{
    const double fps = getApp()->getProjectFrameRate();
    ASSERT_GT(fps, 0.);
    char snippet[128];
    std::snprintf(snippet, sizeof(snippet), "@P.y += @TimeInc * %.17g;", fps);
    NodePtr previous = begin(_cube);
    NodePtr body = wrangle(previous, snippet);
    NodePtr sim = end(body);
    setInt(sim, kGeoSolverEndParamSubsteps, 4);

    EXPECT_NEAR(4., rise(sim, 5.), 1e-4);
}

TEST_F(GeoSolverTest, InputBeginsPassTheCurrentFrameThrough)
{
    NodePtr previous = begin(_cube);
    NodePtr live = begin(_cube, eGeoSolverBeginInput);
    NodePtr body = wrangle(previous, "@P.y += 1;");
    connectNodes(live, body, 1, true);
    NodePtr sim = end(body);

    EXPECT_NEAR(4., rise(sim, 5.), 1e-4);
    ASSERT_EQ(2u, solver(sim)->getBegins().size());
}

TEST_F(GeoSolverTest, NoPrevFrameBeginIsAnError)
{
    NodePtr live = begin(_cube, eGeoSolverBeginInput);
    NodePtr sim = end(transform(live));
    GeoDetailConstPtr result;
    std::string error;

    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, pull(sim, 5., &result, &error));
}

TEST_F(GeoSolverTest, OneAndEightThreadsGiveTheSameResult)
{
    NodePtr previous = begin(_cube);
    NodePtr body = wrangle(previous, "@P += {0.1, 1, 0} * (1 + @ptnum % 3) + sin(@P.x * 3.7);");
    NodePtr sim = end(body);
    setInt(sim, kGeoSolverEndParamSubsteps, 3);

    U64 one = 0;
    U64 eight = 0;
    {
        AppTLS::ThreadBudgetScope budget(1);
        one = contentHash(sim, 20.);
    }
    solver(sim)->resetSimulation();
    {
        AppTLS::ThreadBudgetScope budget(8);
        eight = contentHash(sim, 20.);
    }
    EXPECT_NE(0u, one);
    EXPECT_EQ(one, eight);
    EXPECT_EQ(38u, solver(sim)->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, TheSolverNeedsOrderedFrames)
{
    NodePtr sim = risingSolver(1.);
    std::shared_ptr<NativeEffectBase> native = std::dynamic_pointer_cast<NativeEffectBase>(sim->getEffectInstance());

    ASSERT_TRUE(native != NULL);
    EXPECT_TRUE(native->needsOrderedFrames());
}

// The solver inside a loop keys its frames on the loop's context: each iteration simulates on its own.
TEST_F(GeoSolverTest, ASolverInsideAForEachSimulatesEachIteration)
{
    NodePtr input = add(PLUGINID_NATRON_GEOFOREACHBEGIN);
    ASSERT_TRUE(input != NULL);
    connectNodes(_cube, input, 0, true);
    setChoice(input, kGeoForEachBeginParamMethod, (int)eGeoForEachBeginFetchInput);
    NodePtr previous = begin(input);
    NodePtr move = transform(previous);
    // loop.iteration is the solver's own: the for-each's is read by its name.
    drive(move, kGeoTransformParamTranslate, 1, "loops.pieces.iteration", 1., 2.);
    NodePtr sim = end(move);
    NodePtr loop = add(PLUGINID_NATRON_GEOFOREACHEND);
    ASSERT_TRUE(loop != NULL);
    setString(loop, kGeoForEachEndParamLoopName, "pieces");
    connectNodes(sim, loop, 0, true);
    setChoice(loop, kGeoForEachEndParamIterationMethod, (int)eGeoForEachIterateCount);
    setInt(loop, kGeoForEachEndParamIterations, 2);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, 5., &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    const std::vector<float> p = positions(*result);
    const std::size_t n = cube().size() / 3;
    ASSERT_EQ(2 * n * 3, p.size());
    EXPECT_NEAR(4., meanY(p, 0, n) - meanY(cube()), 1e-4);
    EXPECT_NEAR(8., meanY(p, n, n) - meanY(cube()), 1e-4);
}

// A body moving y by variable each substep, start 0 and end 1 mapping the variable to itself.
TEST_F(GeoSolverTest, LoopFrameIsTheFrameEachSubstepSolves)
{
    NodePtr previous = begin(_cube);
    NodePtr move = transform(previous);
    drive(move, kGeoTransformParamTranslate, 1, "loop.frame", 0., 1.);
    NodePtr sim = end(move);

    EXPECT_NEAR(2. + 3. + 4. + 5., rise(sim, 5.), 1e-4);
}

TEST_F(GeoSolverTest, LoopIterationCountsTheSubstepsSinceTheStartFrame)
{
    NodePtr previous = begin(_cube);
    NodePtr move = transform(previous);
    drive(move, kGeoTransformParamTranslate, 1, "loop.iteration", 0., 1.);
    NodePtr sim = end(move);
    setInt(sim, kGeoSolverEndParamSubsteps, 2);

    EXPECT_NEAR(0. + 1. + 2. + 3., rise(sim, 3.), 1e-4);
}

// A walk resuming from a kept frame counts on from the start frame, not from where it resumed.
TEST_F(GeoSolverTest, LoopIterationIsTheSameWhicheverFrameWasAskedFirst)
{
    NodePtr previous = begin(_cube);
    NodePtr move = transform(previous);
    drive(move, kGeoTransformParamTranslate, 1, "loop.iteration", 0., 1.);
    NodePtr sim = end(move);

    EXPECT_NEAR(0. + 1., rise(sim, 3.), 1e-4);
    EXPECT_NEAR(0. + 1. + 2. + 3., rise(sim, 5.), 1e-4);
}

TEST_F(GeoSolverTest, LoopCountAndNumIterationsAreTheSubstepsToTheEndOfTheFrame)
{
    for (const char* variable : { "loop.count", "loop.numiterations" }) {
        NodePtr previous = begin(_cube);
        NodePtr move = transform(previous);
        drive(move, kGeoTransformParamTranslate, 1, variable, 0., 1.);
        NodePtr sim = end(move);
        setInt(sim, kGeoSolverEndParamSubsteps, 2);

        EXPECT_NEAR(2. + 2. + 4. + 4., rise(sim, 3.), 1e-4) << variable;
    }
}

TEST_F(GeoSolverTest, LoopTIsThePartOfTheFrameSolved)
{
    NodePtr previous = begin(_cube);
    NodePtr move = transform(previous);
    drive(move, kGeoTransformParamTranslate, 1, "loop.t", 0., 1.);
    NodePtr sim = end(move);
    setInt(sim, kGeoSolverEndParamSubsteps, 4);

    EXPECT_NEAR(0.25 + 0.5 + 0.75 + 1., rise(sim, 2.), 1e-4);
}

TEST_F(GeoSolverTest, LoopValueIsTheTimeASubstepSolvesTo)
{
    NodePtr previous = begin(_cube);
    NodePtr move = transform(previous);
    drive(move, kGeoTransformParamTranslate, 1, "loop.value", 0., 1.);
    NodePtr sim = end(move);
    setInt(sim, kGeoSolverEndParamSubsteps, 2);

    EXPECT_NEAR(1.5 + 2. + 2.5 + 3., rise(sim, 3.), 1e-4);
}

TEST_F(GeoSolverTest, TheSolverSetsItsVariablesUnderItsName)
{
    NodePtr previous = begin(_cube);
    NodePtr move = transform(previous);
    drive(move, kGeoTransformParamTranslate, 1, "loops.sim.substep", 0., 1.);
    NodePtr sim = end(move);
    setString(sim, kGeoSolverEndParamLoopName, "sim");
    setInt(sim, kGeoSolverEndParamSubsteps, 3);

    EXPECT_NEAR(0. + 1. + 2., rise(sim, 2.), 1e-4);
}

TEST_F(GeoSolverTest, TheSolverPublishesTheImageLoopVariables)
{
    const std::vector<std::string>& variables = GeoSolverEnd::loopVariables();

    for (const char* name : { "iteration", "numiterations", "count", "t", "value", "frame", "substep", "numsubsteps", "dt" }) {
        EXPECT_TRUE(std::find(variables.begin(), variables.end(), name) != variables.end()) << name;
    }
    for (const std::string& name : LoopEnd::loopVariables()) {
        EXPECT_TRUE(std::find(variables.begin(), variables.end(), name) != variables.end()) << name;
    }
}

TEST_F(GeoSolverTest, NewSolversAndForEachLoopsTakeTheFirstFreeLoopName)
{
    NodePtr first = risingSolver(1.);
    NodePtr second = risingSolver(1.);
    NodePtr loop = add(PLUGINID_NATRON_GEOFOREACHEND);
    ASSERT_TRUE(first != NULL);
    ASSERT_TRUE(second != NULL);
    ASSERT_TRUE(loop != NULL);

    EXPECT_EQ(std::string("loop1"), solver(first)->getLoopName());
    EXPECT_EQ(std::string("loop2"), solver(second)->getLoopName());
    const std::shared_ptr<GeoForEachEnd> forEach = std::dynamic_pointer_cast<GeoForEachEnd>(loop->getEffectInstance());
    ASSERT_TRUE(forEach != NULL);
    EXPECT_EQ(std::string("loop3"), forEach->getLoopName());
}

TEST_F(GeoSolverTest, TheSolverPreviewsTheLastSubstepOfTheFrame)
{
    NodePtr sim = risingSolver(1.);
    setInt(sim, kGeoSolverEndParamSubsteps, 4);
    setString(sim, kGeoSolverEndParamLoopName, "sim");

    const RenderContextPtr context = solver(sim)->getBlockPreviewContext(RenderContextPtr(), 7.);
    ASSERT_TRUE(context != NULL);
    const RenderContextValue* frame = context->find(RenderContextName::loops("sim", "frame"));
    const RenderContextValue* substep = context->find(RenderContextName::loop("substep"));
    ASSERT_TRUE(frame != NULL);
    ASSERT_TRUE(substep != NULL);
    EXPECT_EQ((std::int64_t)7, std::get<std::int64_t>(*frame));
    EXPECT_EQ((std::int64_t)3, std::get<std::int64_t>(*substep));
}

TEST_F(GeoSolverTest, TheKeptFramesAreThoseOfTheSeriesLastRendered)
{
    NodePtr sim = risingSolver(1.);

    EXPECT_TRUE(solver(sim)->getKeptFrames().empty());
    EXPECT_NEAR(4., rise(sim, 5.), 1e-4);
    const std::map<int, bool> kept = solver(sim)->getKeptFrames();
    for (int frame = 1; frame <= 5; ++frame) {
        EXPECT_EQ(1u, kept.count(frame)) << frame;
    }
    EXPECT_FALSE(solver(sim)->outputKeptFrame(5));
    EXPECT_NEAR(2., rise(sim, 3.), 1e-4);
    EXPECT_TRUE(solver(sim)->outputKeptFrame(3));
    solver(sim)->resetSimulation();
    EXPECT_TRUE(solver(sim)->getKeptFrames().empty());
}

TEST_F(GeoSolverTest, AnEarlierFrameAfterALaterOneIsNotSolvedAgain)
{
    NodePtr sim = risingSolver(1.);

    EXPECT_NEAR(49., rise(sim, 50.), 1e-3);
    EXPECT_EQ(49u, solver(sim)->getSolvedFrameCount());
    EXPECT_NEAR(19., rise(sim, 20.), 1e-3);
    EXPECT_EQ(49u, solver(sim)->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, ALaterFrameSolvesOnFromTheLastKeptOne)
{
    NodePtr sim = risingSolver(1.);

    EXPECT_NEAR(9., rise(sim, 10.), 1e-3);
    EXPECT_NEAR(19., rise(sim, 20.), 1e-3);
    EXPECT_EQ(19u, solver(sim)->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, AnEditAboveSolvesAgainFromTheStart)
{
    NodePtr move;
    NodePtr sim = risingSolver(1., &move);

    EXPECT_NEAR(19., rise(sim, 20.), 1e-3);
    EXPECT_EQ(19u, solver(sim)->getSolvedFrameCount());
    setDouble(move, kGeoTransformParamTranslate, 2., 1);
    EXPECT_NEAR(38., rise(sim, 20.), 1e-3);
    EXPECT_EQ(38u, solver(sim)->getSolvedFrameCount());
    // Only the edited simulation's frames are kept.
    EXPECT_EQ(20u, solver(sim)->getSimCache().ramFrameCount());
}

TEST_F(GeoSolverTest, ResetSimulationSolvesAgainFromTheStart)
{
    NodePtr sim = risingSolver(1.);

    EXPECT_NEAR(9., rise(sim, 10.), 1e-3);
    solver(sim)->resetSimulation();
    EXPECT_EQ(0u, solver(sim)->getSimCache().ramFrameCount());
    EXPECT_NEAR(9., rise(sim, 10.), 1e-3);
    EXPECT_EQ(18u, solver(sim)->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, WithoutCacheSimulationOnlyTheLastFrameIsKept)
{
    NodePtr sim = risingSolver(1.);

    setBool(sim, kGeoSolverEndParamCacheSimulation, false);
    EXPECT_NEAR(9., rise(sim, 10.), 1e-3);
    EXPECT_EQ(1u, solver(sim)->getSimCache().ramFrameCount());
    EXPECT_NEAR(11., rise(sim, 12.), 1e-3);
    EXPECT_EQ(11u, solver(sim)->getSolvedFrameCount());
    EXPECT_NEAR(4., rise(sim, 5.), 1e-3);
    EXPECT_EQ(15u, solver(sim)->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, ATinyBudgetSpillsFramesAndReadsThemBackUnchanged)
{
    NodePtr sim = risingSolver(1.);
    std::vector<U64> kept;

    for (int f = 1; f <= 10; ++f) {
        kept.push_back(contentHash(sim, (double)f));
    }
    setInt(sim, kGeoSolverEndParamCacheMemory, 0);
    solver(sim)->resetSimulation();
    EXPECT_NE(0u, contentHash(sim, 10.));
    const std::uint64_t solved = solver(sim)->getSolvedFrameCount();
    EXPECT_EQ(18u, solved);
    EXPECT_LE(solver(sim)->getSimCache().ramFrameCount(), 1u);
    EXPECT_GE(solver(sim)->getSimCache().spilledFrameCount(), 9u);
    EXPECT_FALSE(solver(sim)->getSimCache().spillDirectory().empty());
    for (int f = 1; f <= 10; ++f) {
        EXPECT_EQ(kept[(std::size_t)f - 1], contentHash(sim, (double)f)) << "frame " << f;
    }
    EXPECT_EQ(solved, solver(sim)->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, WithoutSpillToDiskATinyBudgetSolvesAgain)
{
    NodePtr sim = risingSolver(1.);

    setInt(sim, kGeoSolverEndParamCacheMemory, 0);
    setBool(sim, kGeoSolverEndParamSpillToDisk, false);
    EXPECT_NEAR(9., rise(sim, 10.), 1e-3);
    EXPECT_EQ(0u, solver(sim)->getSimCache().spilledFrameCount());
    EXPECT_TRUE(solver(sim)->getSimCache().spillDirectory().empty());
    EXPECT_NEAR(4., rise(sim, 5.), 1e-3);
    EXPECT_EQ(13u, solver(sim)->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, ACancelledFrameIsNotKeptAndTheNextRequestResumes)
{
    const int last = 3000;
    NodePtr sim = risingSolver(0.25);
    setInt(sim, kGeoSolverEndParamSubsteps, 4);
    const std::shared_ptr<GeoSolverEnd> effect = solver(sim);

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    std::atomic<bool> done(false);
    std::thread aborter([&]() {
        while (!done.load() && (effect->getSolvedFrameCount() < 20)) {
            std::this_thread::yield();
        }
        // Most likely between two substeps of the frame after.
        abortInfo->setAborted();
    });
    GeoDetailConstPtr result;
    std::string error;
    const EffectInstance::RenderRoIRetCode code = pull(sim, (double)last, &result, &error, true, abortInfo);
    done = true;
    aborter.join();
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, code) << error;
    const std::uint64_t solved = effect->getSolvedFrameCount();
    EXPECT_GE(solved, 20u);
    EXPECT_LT(solved, (std::uint64_t)(last - 1));
    // The start frame and every whole frame, but not the one the abort cut short.
    EXPECT_EQ(solved + 1, effect->getSimCache().ramFrameCount());
    EXPECT_EQ(0u, effect->getSimCache().spilledFrameCount());

    EXPECT_NEAR((double)(last - 1), rise(sim, (double)last), 1e-1);
    // Every frame solved once: the second request started where the first stopped.
    EXPECT_EQ((std::uint64_t)(last - 1), effect->getSolvedFrameCount());
}

TEST_F(GeoSolverTest, AKeptFrameIsShownWhileAnotherRequestWalksOn)
{
    const int last = 100000;
    NodePtr sim = risingSolver(1.);
    const std::shared_ptr<GeoSolverEnd> effect = solver(sim);

    EXPECT_NEAR(9., rise(sim, 10.), 1e-3);
    ASSERT_EQ(9u, effect->getSolvedFrameCount());

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
    std::thread walker([&]() {
        GeoDetailConstPtr result;
        std::string error;
        code = pull(sim, (double)last, &result, &error, true, abortInfo);
    });
    const std::chrono::steady_clock::time_point waitStart = std::chrono::steady_clock::now();
    while ((effect->getSolvedFrameCount() < 30) && (std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(30))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::uint64_t walkedBefore = effect->getSolvedFrameCount();
    const std::chrono::steady_clock::time_point askedAt = std::chrono::steady_clock::now();
    const double kept = rise(sim, 5.);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - askedAt).count();
    const std::uint64_t walkedAfter = effect->getSolvedFrameCount();
    abortInfo->setAborted();
    walker.join();

    EXPECT_GE(walkedBefore, 30u) << "the long request never started walking";
    EXPECT_NEAR(4., kept, 1e-3);
    EXPECT_LT(ms, 500.) << "the kept frame waited " << ms << " ms for the walk";
    EXPECT_LT(walkedAfter, (std::uint64_t)(last - 1)) << "the walk finished before the kept frame was asked for";
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, code);
}

TEST_F(GeoSolverTest, AnEditHeldBackByALongSolveAbortsIt)
{
    struct QueueWritesScope {
        QueueWritesScope() { EffectInstance::setQueuesWritesDuringBackgroundRendersForTesting(true); }
        ~QueueWritesScope() { EffectInstance::setQueuesWritesDuringBackgroundRendersForTesting(false); }
    };

    for (int editBody = 0; editBody < 2; ++editBody) {
        SCOPED_TRACE(editBody ? "body" : "solver");
        NodePtr move;
        NodePtr sim = risingSolver(1., &move);
        ASSERT_TRUE(sim != NULL);
        KnobIntPtr substeps = std::dynamic_pointer_cast<KnobInt>(sim->getKnobByName(kGeoSolverEndParamSubsteps));
        KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(move->getKnobByName(kGeoTransformParamTranslate));
        ASSERT_TRUE(substeps != NULL);
        ASSERT_TRUE(translate != NULL);

        const QueueWritesScope queueWrites;
        std::unique_ptr<RenderingFlagSetter> rendering(new RenderingFlagSetter(sim));
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
        std::string error;
        std::chrono::steady_clock::time_point returnedAt;
        std::thread cook([&]() {
            GeoDetailConstPtr result;
            code = pull(sim, 100000., &result, &error, true, abortInfo);
            returnedAt = std::chrono::steady_clock::now();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::chrono::steady_clock::time_point editedAt = std::chrono::steady_clock::now();
        if (editBody) {
            translate->setValue(2., ViewSpec::all(), 1);
        } else {
            substeps->setValue(2);
        }
        const bool heldBack = editBody ? (translate->getRawValue(1) == 1.) : (substeps->getRawValue(0) == 1);
        cook.join();
        rendering.reset();

        EXPECT_TRUE(heldBack) << "the edit applied while the solver rendered";
        if ((code == EffectInstance::eRenderRoIRetCodeOk) && (returnedAt <= editedAt)) {
            // A machine fast enough to finish before the edit has nothing left to abort.
            continue;
        }
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, code) << error;
        EXPECT_TRUE(abortInfo->isAborted());
        const double ms = std::chrono::duration<double, std::milli>(returnedAt - editedAt).count();
        EXPECT_LT(ms, 2000.) << "the solve went on for " << ms << " ms after the edit";
    }
}

TEST_F(GeoSolverTest, ASourceFailureAtTheStartFrameIsAnError)
{
    NodePtr previous = begin(NodePtr());
    NodePtr bad = wrangle(_cube, "@P.y += ;");
    connectNodes(bad, previous, 0, true);
    NodePtr move = transform(previous);
    NodePtr sim = end(move);

    GeoDetailConstPtr result;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, pull(sim, 5., &result, &error));
    EXPECT_NE(std::string::npos, error.find(sim->getScriptName_mt_safe())) << error;
}

TEST_F(GeoSolverTest, LoweringTheBudgetEvictsAtTheNextInsertion)
{
    GeoDetailConstPtr detail;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_cube, 1., &detail, &error)) << error;
    ASSERT_TRUE(detail != NULL);
    ASSERT_GT(detail->getSizeInBytes(), 0u);
    GeoSimCache cache(detail->getSizeInBytes() * 16);
    GeoSimSeries series;
    series.hash = 1;

    for (int f = 1; f <= 4; ++f) {
        cache.insert(series, f, detail);
    }
    EXPECT_EQ(4u, cache.ramFrameCount());
    cache.setBudget(0);
    EXPECT_EQ(4u, cache.ramFrameCount());
    cache.insert(series, 5, detail);
    // Only the frame just inserted stays; there is no spill directory.
    EXPECT_EQ(1u, cache.ramFrameCount());
    EXPECT_TRUE(cache.find(series, 5) != NULL);
    EXPECT_TRUE(cache.find(series, 1) == NULL);
    cache.setBudget(detail->getSizeInBytes() * 2);
    cache.insert(series, 6, detail);
    // Read after 6 was inserted, 5 outlives it: 6 is then the least recently used.
    EXPECT_TRUE(cache.find(series, 5) != NULL);
    cache.insert(series, 7, detail);
    EXPECT_TRUE(cache.find(series, 5) != NULL);
    EXPECT_TRUE(cache.find(series, 6) == NULL);
    EXPECT_TRUE(cache.find(series, 7) != NULL);
}

TEST_F(GeoSolverTest, SpillDirectoriesOfDeadProcessesAreRemoved)
{
    const std::filesystem::path parent = std::filesystem::temp_directory_path() / ("natron-geosolver-test-" + std::to_string((long)::getpid()));
    const std::filesystem::path dead = parent / "2147483600-0";
    const std::filesystem::path own = parent / (std::to_string((long)::getpid()) + "-3");
    const std::filesystem::path other = parent / "notapid";
    std::error_code ec;

    std::filesystem::create_directories(dead, ec);
    std::filesystem::create_directories(own, ec);
    std::filesystem::create_directories(other, ec);
    std::FILE* file = std::fopen((dead / "frame.ngeo").string().c_str(), "wb");
    ASSERT_TRUE(file != NULL);
    std::fclose(file);

    GeoSimCache::removeOrphanedSpills(parent.string());
    EXPECT_FALSE(std::filesystem::exists(dead));
    EXPECT_TRUE(std::filesystem::exists(own));
    EXPECT_TRUE(std::filesystem::exists(other));
    std::filesystem::remove_all(parent, ec);
}
