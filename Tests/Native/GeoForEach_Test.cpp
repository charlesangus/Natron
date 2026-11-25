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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

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
#include "Engine/Nodes/Geometry/GeoCopyToPoints.h"
#include "Engine/Nodes/Geometry/GeoMerge.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoTransform.h"
#include "Engine/Nodes/Geometry/GeoWrangle.h"
#include "Engine/Nodes/Loop/GeoForEachBegin.h"
#include "Engine/Nodes/Loop/GeoForEachEnd.h"
#include "Engine/Nodes/Loop/GeoForEachPieces.h"
#include "Engine/ParallelRenderArgs.h"
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

void
centroid(const std::vector<float>& p,
         double c[3])
{
    c[0] = c[1] = c[2] = 0.;
    const std::size_t n = p.size() / 3;
    for (std::size_t i = 0; i < n; ++i) {
        for (int d = 0; d < 3; ++d) {
            c[d] += p[3 * i + d];
        }
    }
    for (int d = 0; d < 3; ++d) {
        c[d] = n ? c[d] / (double)n : 0.;
    }
}

std::int64_t
intAt(const GeoAttribute& attribute,
      std::size_t offset)
{
    return (attribute.storage() == eGeoStorageInt64) ? attribute.get<std::int64_t>(offset) : (std::int64_t)attribute.get<std::int32_t>(offset);
}

// NATRON_GEOFOREACH_BENCH=1 runs PiecesBench over 10,000 pieces; normal runs use 100 so they
// stay quick. tools/bench/geo/foreach_pieces.py runs it and reads the lines it prints.
bool
fullBench()
{
    const char* value = std::getenv("NATRON_GEOFOREACH_BENCH");

    return value && (value[0] != '\0') && (std::strcmp(value, "0") != 0);
}

} // namespace

class GeoForEachTest
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
        // The default cube splits each side into 2x2 faces; the piece counts assume one face per side.
        for (int d = 0; d < 3; ++d) {
            setInt(_cube, kGeoPrimitiveParamCubeSubdivisions, 1, d);
        }
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
                  GeoForEachBeginMethodEnum method)
    {
        NodePtr node = add(PLUGINID_NATRON_GEOFOREACHBEGIN);
        if (node) {
            if (input) {
                connectNodes(input, node, 0, true);
            }
            setChoice(node, kGeoForEachBeginParamMethod, (int)method);
        }

        return node;
    }

    NodePtr end(const NodePtr& body,
                GeoForEachIterationEnum iteration,
                GeoForEachGatherEnum gather)
    {
        NodePtr node = add(PLUGINID_NATRON_GEOFOREACHEND);
        if (node) {
            connectNodes(body, node, 0, true);
            setChoice(node, kGeoForEachEndParamIterationMethod, (int)iteration);
            setChoice(node, kGeoForEachEndParamGatherMethod, (int)gather);
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
                    const std::string& snippet,
                    int runOver)
    {
        NodePtr node = add(PLUGINID_NATRON_GEOWRANGLE);
        if (!node) {
            return node;
        }
        if (input) {
            connectNodes(input, node, 0, true);
        }
        setString(node, kGeoWrangleParamSnippet, snippet);
        setChoice(node, kGeoWrangleParamRunOver, runOver);

        return node;
    }

    // Three cubes 10 apart along x, their primitives named a, b and c.
    NodePtr threeBoxes()
    {
        NodePtr merge = add(PLUGINID_NATRON_GEOMERGE);
        const char* const names[] = { "a", "b", "c" };
        for (int k = 0; k < 3; ++k) {
            NodePtr moved = transform(_cube);
            setDouble(moved, kGeoTransformParamTranslate, 10. * k, 0);
            NodePtr named = wrangle(moved, std::string("s@name = \"") + names[k] + "\";", 1);
            connectNodes(named, merge, k, true);
        }

        return merge;
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
                       int value,
                       int dimension = 0)
    {
        KnobIntPtr knob = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value, ViewSpec::all(), dimension);
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
                                          GeoDetailConstPtr* detail,
                                          std::string* error,
                                          bool byPassCache = false,
                                          AbortableRenderInfoPtr abortInfo = AbortableRenderInfoPtr())
    {
        const double time = 1.;
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

    std::vector<float> cubePositions()
    {
        GeoDetailConstPtr source;
        std::string error;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_cube, &source, &error)) << error;

        return source ? positions(*source) : std::vector<float>();
    }

    // Each copy k of the cube's points offset along x by offset(k), copy after copy.
    static void expectOffsetCopies(const std::vector<float>& cube,
                                   const std::vector<float>& result,
                                   int copies,
                                   double step)
    {
        ASSERT_FALSE(cube.empty());
        ASSERT_EQ(cube.size() * (std::size_t)copies, result.size());
        for (int k = 0; k < copies; ++k) {
            for (std::size_t i = 0; i < cube.size(); i += 3) {
                const std::size_t j = (std::size_t)k * cube.size() + i;
                EXPECT_NEAR(cube[i] + step * k, result[j], 1e-5) << "copy " << k << " point " << i / 3;
                EXPECT_NEAR(cube[i + 1], result[j + 1], 1e-5) << "copy " << k << " point " << i / 3;
                EXPECT_NEAR(cube[i + 2], result[j + 2], 1e-5) << "copy " << k << " point " << i / 3;
            }
        }
    }

    // GeoForEachBegin (Extract) on three boxes -> GeoTransform scaling by 2 about the piece's
    // centroid -> GeoForEachEnd iterating over pieces split by method.
    void expectBoxesScaledAboutTheirCentroids(GeoPieceMethodEnum method)
    {
        const std::vector<float> cube = cubePositions();
        double c[3];
        centroid(cube, c);

        NodePtr boxes = threeBoxes();
        NodePtr piece = begin(boxes, eGeoForEachBeginExtractPiece);
        NodePtr scale = transform(piece);
        setDouble(scale, kGeoTransformParamUniformScale, 2., 0);
        drive(scale, kGeoTransformParamPivot, 0, "loop.value", c[0], c[0] + 10.);
        setDouble(scale, kGeoTransformParamPivot, c[1], 1);
        setDouble(scale, kGeoTransformParamPivot, c[2], 2);
        NodePtr loop = end(scale, eGeoForEachIteratePieces, eGeoForEachGatherMerge);
        setChoice(loop, kGeoForEachEndParamPieceMethod, (int)method);
        setString(loop, kGeoForEachEndParamPieceAttribute, "name");

        GeoDetailConstPtr result;
        std::string error;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error)) << error;
        ASSERT_TRUE(result != NULL);
        const std::vector<float> after = positions(*result);
        ASSERT_EQ(cube.size() * 3, after.size());
        for (int k = 0; k < 3; ++k) {
            for (std::size_t i = 0; i < cube.size(); i += 3) {
                const std::size_t j = (std::size_t)k * cube.size() + i;
                EXPECT_NEAR(c[0] + 10. * k + 2. * (cube[i] - c[0]), after[j], 1e-4) << "box " << k << " point " << i / 3;
                EXPECT_NEAR(c[1] + 2. * (cube[i + 1] - c[1]), after[j + 1], 1e-4) << "box " << k << " point " << i / 3;
                EXPECT_NEAR(c[2] + 2. * (cube[i + 2] - c[2]), after[j + 2], 1e-4) << "box " << k << " point " << i / 3;
            }
        }
    }

    NodePtr _cube;
    std::vector<NodePtr> _nodes;
};

TEST_F(GeoForEachTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_GEOFOREACHBEGIN, 1, "GeoForEachBegin");
    expectNativeIdentity(PLUGINID_NATRON_GEOFOREACHEND, 1, "GeoForEachEnd");
}

TEST_F(GeoForEachTest, CountFiveMergesFiveOffsetCopies)
{
    NodePtr input = begin(_cube, eGeoForEachBeginFetchInput);
    NodePtr move = transform(input);
    drive(move, kGeoTransformParamTranslate, 0, "loop.iteration", 0., 10.);
    NodePtr loop = end(move, eGeoForEachIterateCount, eGeoForEachGatherMerge);
    setInt(loop, kGeoForEachEndParamIterations, 5);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    expectOffsetCopies(cubePositions(), positions(*result), 5, 10.);
}

TEST_F(GeoForEachTest, FeedbackFiveAppliesTheBodyFiveTimes)
{
    NodePtr previous = begin(_cube, eGeoForEachBeginFetchFeedback);
    NodePtr move = transform(previous);
    setDouble(move, kGeoTransformParamTranslate, 1., 0);
    NodePtr loop = end(move, eGeoForEachIterateCount, eGeoForEachGatherFeedback);
    setInt(loop, kGeoForEachEndParamIterations, 5);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    const std::vector<float> cube = cubePositions();
    std::vector<float> expected = cube;
    for (std::size_t i = 0; i < expected.size(); i += 3) {
        expected[i] += 5.f;
    }
    expectOffsetCopies(expected, positions(*result), 1, 0.);
}

TEST_F(GeoForEachTest, StopConditionEndsTheLoopEarly)
{
    NodePtr input = begin(_cube, eGeoForEachBeginFetchInput);
    NodePtr move = transform(input);
    drive(move, kGeoTransformParamTranslate, 0, "loop.iteration", 0., 10.);
    NodePtr loop = end(move, eGeoForEachIterateCount, eGeoForEachGatherMerge);
    setInt(loop, kGeoForEachEndParamIterations, 10);
    setString(loop, kGeoForEachEndParamStopCondition, "loop.iteration >= 3");

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    expectOffsetCopies(cubePositions(), positions(*result), 3, 10.);
}

TEST_F(GeoForEachTest, SinglePassCooksOneIteration)
{
    NodePtr input = begin(_cube, eGeoForEachBeginFetchInput);
    NodePtr move = transform(input);
    drive(move, kGeoTransformParamTranslate, 0, "loop.iteration", 0., 10.);
    NodePtr loop = end(move, eGeoForEachIterateCount, eGeoForEachGatherMerge);
    setInt(loop, kGeoForEachEndParamIterations, 5);
    setBool(loop, kGeoForEachEndParamSinglePass, true);
    setInt(loop, kGeoForEachEndParamSinglePassIteration, 2);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    std::vector<float> expected = cubePositions();
    for (std::size_t i = 0; i < expected.size(); i += 3) {
        expected[i] += 20.f;
    }
    expectOffsetCopies(expected, positions(*result), 1, 0.);
}

TEST_F(GeoForEachTest, OneAndEightThreadsGiveTheSameResult)
{
    NodePtr boxes = threeBoxes();
    NodePtr piece = begin(boxes, eGeoForEachBeginExtractPiece);
    NodePtr move = transform(piece);
    drive(move, kGeoTransformParamTranslate, 1, "loop.iteration", 0., 3.);
    NodePtr loop = end(move, eGeoForEachIteratePieces, eGeoForEachGatherMerge);
    setChoice(loop, kGeoForEachEndParamPieceMethod, (int)eGeoPieceMethodEachElement);

    GeoDetailConstPtr one;
    GeoDetailConstPtr eight;
    std::string error;
    {
        AppTLS::ThreadBudgetScope budget(1);
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &one, &error, true)) << error;
    }
    {
        AppTLS::ThreadBudgetScope budget(8);
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &eight, &error, true)) << error;
    }
    ASSERT_TRUE(one != NULL);
    ASSERT_TRUE(eight != NULL);
    EXPECT_GT(one->primitiveCount(), 3);
    EXPECT_EQ(positions(*one), positions(*eight));
    EXPECT_EQ(one->contentHash(), eight->contentHash());
}

// Helper threads need independent frame args because the calling thread pushes and pops
// body-effect args while cooking its own iterations.
TEST_F(GeoForEachTest, ManyIterationsMergeOverSeveralThreads)
{
    const int iterations = 20000;
    NodePtr input = begin(_cube, eGeoForEachBeginFetchInput);
    NodePtr move = transform(input);
    drive(move, kGeoTransformParamTranslate, 0, "loop.iteration", 0., 1.);
    NodePtr loop = end(move, eGeoForEachIterateCount, eGeoForEachGatherMerge);
    setInt(loop, kGeoForEachEndParamIterations, iterations);

    AppTLS::ThreadBudgetScope budget(8);
    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error, true)) << error;
    ASSERT_TRUE(result != NULL);
    const std::vector<float> cube = cubePositions();
    ASSERT_FALSE(cube.empty());
    EXPECT_EQ((GeoIndex)iterations * 6, result->primitiveCount());
    EXPECT_EQ(cube.size() / 3 * (std::size_t)iterations, (std::size_t)result->pointCount());
}

TEST_F(GeoForEachTest, ManyFeedbackIterationsApplyTheBodyEachTime)
{
    const int iterations = 10000;
    NodePtr previous = begin(_cube, eGeoForEachBeginFetchFeedback);
    NodePtr move = transform(previous);
    setDouble(move, kGeoTransformParamTranslate, 1., 0);
    NodePtr loop = end(move, eGeoForEachIterateCount, eGeoForEachGatherFeedback);
    setInt(loop, kGeoForEachEndParamIterations, iterations);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error, true)) << error;
    ASSERT_TRUE(result != NULL);
    std::vector<float> expected = cubePositions();
    for (std::size_t i = 0; i < expected.size(); i += 3) {
        expected[i] += (float)iterations;
    }
    const std::vector<float> after = positions(*result);
    ASSERT_EQ(expected.size(), after.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(expected[i], after[i], 1e-2) << "coordinate " << i;
    }
}

TEST_F(GeoForEachTest, AbortingTheMaximumIterationCountReturnsPromptly)
{
    for (int gather = 0; gather < 2; ++gather) {
        SCOPED_TRACE(gather ? "feedback" : "merge");
        NodePtr input = begin(_cube, gather ? eGeoForEachBeginFetchFeedback : eGeoForEachBeginFetchInput);
        NodePtr move = transform(input);
        drive(move, kGeoTransformParamTranslate, 0, "loop.iteration", 0., 1.);
        NodePtr loop = end(move, eGeoForEachIterateCount, gather ? eGeoForEachGatherFeedback : eGeoForEachGatherMerge);
        setInt(loop, kGeoForEachEndParamIterations, kGeoForEachEndDefaultMaxIterations);

        AppTLS::ThreadBudgetScope budget(8);
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        std::chrono::steady_clock::time_point abortedAt;
        std::thread aborter([&abortInfo, &abortedAt]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            abortedAt = std::chrono::steady_clock::now();
            abortInfo->setAborted();
        });
        GeoDetailConstPtr result;
        std::string error;
        const EffectInstance::RenderRoIRetCode code = pull(loop, &result, &error, true, abortInfo);
        const std::chrono::steady_clock::time_point returnedAt = std::chrono::steady_clock::now();
        aborter.join();
        if (code == EffectInstance::eRenderRoIRetCodeOk) {
            // A machine fast enough to finish before the abort still has to get every iteration.
            ASSERT_TRUE(result != NULL);
            EXPECT_EQ((GeoIndex)(gather ? 1 : kGeoForEachEndDefaultMaxIterations) * 6, result->primitiveCount());
        } else {
            EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, code) << error;
            const double ms = std::chrono::duration<double, std::milli>(returnedAt - abortedAt).count();
            EXPECT_LT(ms, 2000.) << "the loop went on for " << ms << " ms after the abort";
        }
    }
}

// An edit made while a viewer cooks the loop is held back until the renders reading the edited
// node end, so no hash moves while the cook runs. Whether of the loop's own knobs or of its body,
// it still aborts the cook within moments.
TEST_F(GeoForEachTest, AnEditHeldBackByALongCookAbortsIt)
{
    struct QueueWritesScope {
        QueueWritesScope() { EffectInstance::setQueuesWritesDuringBackgroundRendersForTesting(true); }
        ~QueueWritesScope() { EffectInstance::setQueuesWritesDuringBackgroundRendersForTesting(false); }
    };

    for (int editBody = 0; editBody < 2; ++editBody) {
        SCOPED_TRACE(editBody ? "body" : "loop");
        NodePtr input = begin(_cube, eGeoForEachBeginFetchFeedback);
        NodePtr move = transform(input);
        drive(move, kGeoTransformParamTranslate, 0, "loop.iteration", 0., 1.);
        NodePtr loop = end(move, eGeoForEachIterateCount, eGeoForEachGatherFeedback);
        setInt(loop, kGeoForEachEndParamIterations, kGeoForEachEndDefaultMaxIterations);
        ASSERT_TRUE(loop != NULL);
        KnobIntPtr iterations = std::dynamic_pointer_cast<KnobInt>(loop->getKnobByName(kGeoForEachEndParamIterations));
        KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(move->getKnobByName(kGeoTransformParamTranslate));
        ASSERT_TRUE(iterations != NULL);
        ASSERT_TRUE(translate != NULL);

        const QueueWritesScope queueWrites;
        std::unique_ptr<RenderingFlagSetter> rendering(new RenderingFlagSetter(loop));
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
        std::string error;
        std::chrono::steady_clock::time_point returnedAt;
        std::thread cook([&]() {
            GeoDetailConstPtr result;
            code = pull(loop, &result, &error, true, abortInfo);
            returnedAt = std::chrono::steady_clock::now();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::chrono::steady_clock::time_point editedAt = std::chrono::steady_clock::now();
        if (editBody) {
            translate->setValue(1., ViewSpec::all(), 1);
        } else {
            iterations->setValue(kGeoForEachEndDefaultMaxIterations - 1);
        }
        const bool heldBack = editBody ? (translate->getRawValue(1) == 0.) : (iterations->getRawValue(0) == kGeoForEachEndDefaultMaxIterations);
        cook.join();
        rendering.reset();

        EXPECT_TRUE(heldBack) << "the edit applied while the loop rendered";
        if ((code == EffectInstance::eRenderRoIRetCodeOk) && (returnedAt <= editedAt)) {
            // A machine fast enough to finish before the edit has nothing left to abort.
            continue;
        }
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, code) << error;
        EXPECT_TRUE(abortInfo->isAborted());
        const double ms = std::chrono::duration<double, std::milli>(returnedAt - editedAt).count();
        EXPECT_LT(ms, 2000.) << "the loop went on for " << ms << " ms after the edit";
    }
}

// Setting a knob to the value it has while a viewer cooks the loop changes nothing, so no render
// follows it: the cook goes on.
TEST_F(GeoForEachTest, ASameValueEditDuringALongCookLeavesItRunning)
{
    struct QueueWritesScope {
        QueueWritesScope() { EffectInstance::setQueuesWritesDuringBackgroundRendersForTesting(true); }
        ~QueueWritesScope() { EffectInstance::setQueuesWritesDuringBackgroundRendersForTesting(false); }
    };

    for (int editBody = 0; editBody < 2; ++editBody) {
        SCOPED_TRACE(editBody ? "body" : "loop");
        NodePtr input = begin(_cube, eGeoForEachBeginFetchFeedback);
        NodePtr move = transform(input);
        drive(move, kGeoTransformParamTranslate, 0, "loop.iteration", 0., 1.);
        NodePtr loop = end(move, eGeoForEachIterateCount, eGeoForEachGatherFeedback);
        setInt(loop, kGeoForEachEndParamIterations, kGeoForEachEndDefaultMaxIterations);
        ASSERT_TRUE(loop != NULL);
        KnobIntPtr iterations = std::dynamic_pointer_cast<KnobInt>(loop->getKnobByName(kGeoForEachEndParamIterations));
        KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(move->getKnobByName(kGeoTransformParamTranslate));
        ASSERT_TRUE(iterations != NULL);
        ASSERT_TRUE(translate != NULL);

        const QueueWritesScope queueWrites;
        std::unique_ptr<RenderingFlagSetter> rendering(new RenderingFlagSetter(loop));
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
        std::string error;
        std::thread cook([&]() {
            GeoDetailConstPtr result;
            code = pull(loop, &result, &error, true, abortInfo);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (editBody) {
            translate->setValue(0., ViewSpec::all(), 1);
        } else {
            iterations->setValue(kGeoForEachEndDefaultMaxIterations);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const bool abortedByTheEdit = abortInfo->isAborted();
        abortInfo->setAborted();
        cook.join();
        rendering.reset();

        EXPECT_FALSE(abortedByTheEdit) << "setting the value the knob has aborted the cook";
        EXPECT_TRUE((code == EffectInstance::eRenderRoIRetCodeOk) || (code == EffectInstance::eRenderRoIRetCodeAborted)) << error;
        EXPECT_EQ(0., translate->getRawValue(1));
        EXPECT_EQ(kGeoForEachEndDefaultMaxIterations, iterations->getRawValue(0));
    }
}

TEST_F(GeoForEachTest, PiecesByNameAreScaledAboutTheirOwnCentroids)
{
    expectBoxesScaledAboutTheirCentroids(eGeoPieceMethodAttribute);
}

TEST_F(GeoForEachTest, PiecesByConnectivityAreScaledAboutTheirOwnCentroids)
{
    expectBoxesScaledAboutTheirCentroids(eGeoPieceMethodConnectivity);
}

// Boxes 0, 1 and 2 named c, a and b, with ids 7, -2 and 3: pieces run a, b, c and -2, 3, 7.
TEST_F(GeoForEachTest, AttributePiecesAreInAscendingValueOrder)
{
    NodePtr merge = add(PLUGINID_NATRON_GEOMERGE);
    const char* const names[] = { "c", "a", "b" };
    const int ids[] = { 7, -2, 3 };
    for (int k = 0; k < 3; ++k) {
        NodePtr moved = transform(_cube);
        setDouble(moved, kGeoTransformParamTranslate, 10. * k, 0);
        NodePtr named = wrangle(moved, std::string("s@name = \"") + names[k] + "\"; i@id = " + std::to_string(ids[k]) + ";", 1);
        connectNodes(named, merge, k, true);
    }
    GeoDetailConstPtr boxes;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(merge, &boxes, &error)) << error;
    ASSERT_TRUE(boxes != NULL);
    ASSERT_EQ(18, boxes->primitiveCount());

    GeoPieceSettings settings;
    settings.elements = eGeoPieceElementsPrimitives;
    settings.method = eGeoPieceMethodAttribute;
    settings.attribute = "name";
    const GeoPiecePartitionConstPtr byName = GeoForEachPieces::partition(*boxes, settings, &error);
    ASSERT_TRUE(byName.get() != NULL) << error;
    ASSERT_EQ(3u, byName->size());
    const char* const sortedNames[] = { "a", "b", "c" };
    const GeoOffset firstOfBox[] = { GeoOffset(6), GeoOffset(12), GeoOffset(0) };
    for (std::size_t piece = 0; piece < 3; ++piece) {
        EXPECT_EQ(std::string(sortedNames[piece]), byName->stringValue(piece)) << piece;
        EXPECT_EQ((std::ptrdiff_t)piece, byName->find(std::string(sortedNames[piece]))) << piece;
        ASSERT_EQ(6u, byName->elements(piece).size()) << piece;
        EXPECT_EQ(firstOfBox[piece], byName->elements(piece).front()) << piece;
    }

    settings.attribute = "id";
    const GeoPiecePartitionConstPtr byId = GeoForEachPieces::partition(*boxes, settings, &error);
    ASSERT_TRUE(byId.get() != NULL) << error;
    ASSERT_EQ(3u, byId->size());
    const std::int64_t sortedIds[] = { -2, 3, 7 };
    for (std::size_t piece = 0; piece < 3; ++piece) {
        EXPECT_EQ(sortedIds[piece], byId->intValue(piece)) << piece;
        EXPECT_EQ((std::ptrdiff_t)piece, byId->find(sortedIds[piece])) << piece;
        ASSERT_EQ(6u, byId->elements(piece).size()) << piece;
        EXPECT_EQ(firstOfBox[piece], byId->elements(piece).front()) << piece;
    }
}

// Primitives 0 (points 0, 1, 2) and 1 (points 2, 3, 4) share point 2. Each piece keeps the
// edges between its own points, renumbered as its points are.
TEST_F(GeoForEachTest, EachPieceKeepsTheEdgesBetweenItsPoints)
{
    GeoDetailBuilder builder;
    builder.appendPoints(5);
    const std::int32_t counts[] = { 3, 3 };
    const GeoOffset corners[] = { 0, 1, 2, 2, 3, 4 };
    builder.appendPrimitives(eGeoPrimitivePolygon, counts, corners);
    GeoEdgeGroup& group = builder.addEdgeGroup("e");
    group.add(0, 1);
    group.add(1, 2);
    group.add(2, 3);
    group.add(3, 4);
    group.add(0, 4);
    const GeoDetail detail = builder.finish();

    GeoPieceSettings settings;
    settings.elements = eGeoPieceElementsPrimitives;
    settings.method = eGeoPieceMethodEachElement;
    std::string error;
    const GeoPiecePartitionConstPtr byPrimitive = GeoForEachPieces::partition(detail, settings, &error);
    ASSERT_TRUE(byPrimitive.get() != NULL) << error;
    ASSERT_EQ(2u, byPrimitive->size());
    for (std::size_t piece = 0; piece < 2; ++piece) {
        SCOPED_TRACE(piece);
        const GeoDetail extracted = GeoForEachPieces::extract(detail, *byPrimitive, piece);
        const GeoEdgeGroup* edges = extracted.findEdgeGroup("e");
        ASSERT_TRUE(edges != NULL);
        ASSERT_EQ(2u, edges->size());
        EXPECT_EQ(GeoOffset(0), edges->edges()[0].p0);
        EXPECT_EQ(GeoOffset(1), edges->edges()[0].p1);
        EXPECT_EQ(GeoOffset(1), edges->edges()[1].p0);
        EXPECT_EQ(GeoOffset(2), edges->edges()[1].p1);
    }

    settings.elements = eGeoPieceElementsPoints;
    settings.method = eGeoPieceMethodConnectivity;
    const GeoPiecePartitionConstPtr connected = GeoForEachPieces::partition(detail, settings, &error);
    ASSERT_TRUE(connected.get() != NULL) << error;
    ASSERT_EQ(1u, connected->size());
    const GeoDetail whole = GeoForEachPieces::extract(detail, *connected, 0);
    const GeoEdgeGroup* allEdges = whole.findEdgeGroup("e");
    ASSERT_TRUE(allEdges != NULL);
    EXPECT_EQ(5u, allEdges->size());

    settings.method = eGeoPieceMethodEachElement;
    const GeoPiecePartitionConstPtr byPoint = GeoForEachPieces::partition(detail, settings, &error);
    ASSERT_TRUE(byPoint.get() != NULL) << error;
    ASSERT_EQ(5u, byPoint->size());
    const GeoDetail lone = GeoForEachPieces::extract(detail, *byPoint, 2);
    const GeoEdgeGroup* noEdges = lone.findEdgeGroup("e");
    ASSERT_TRUE(noEdges != NULL);
    EXPECT_EQ(0u, noEdges->size());
}

// Boxes 0, 1 and 2 named c, a and b, with ids 7, -2 and 3, iterated by name and then by id. In
// each iteration the body reads iteration, ivalue and svalue from a Fetch Metadata GeoForEachBegin
// into primitive attributes, and moves the piece by loop.iteration along y and by loop.ivalue
// along z. The Stop Condition fails an iteration whose loop.svalue or loop.ivalue is not the
// piece's, which would end the loop early.
TEST_F(GeoForEachTest, PieceValuesReachTheBodyAsVariablesAndMetadata)
{
    NodePtr merge = add(PLUGINID_NATRON_GEOMERGE);
    const char* const names[] = { "c", "a", "b" };
    const int ids[] = { 7, -2, 3 };
    for (int k = 0; k < 3; ++k) {
        NodePtr moved = transform(_cube);
        setDouble(moved, kGeoTransformParamTranslate, 10. * k, 0);
        NodePtr named = wrangle(moved, std::string("s@name = \"") + names[k] + "\"; i@id = " + std::to_string(ids[k]) + ";", 1);
        connectNodes(named, merge, k, true);
    }
    const std::vector<float> cube = cubePositions();
    ASSERT_FALSE(cube.empty());
    double c[3];
    centroid(cube, c);
    const std::size_t pointsPerBox = cube.size() / 3;

    for (int byId = 0; byId < 2; ++byId) {
        SCOPED_TRACE(byId ? "id" : "name");
        NodePtr piece = begin(merge, eGeoForEachBeginExtractPiece);
        NodePtr metadata = begin(merge, eGeoForEachBeginFetchMetadata);
        NodePtr read = wrangle(piece,
                               "int it = detail(1, \"iteration\"); int iv = detail(1, \"ivalue\"); string sv = detail(1, \"svalue\"); "
                               "i@gotIteration = it; i@gotIvalue = iv; s@gotSvalue = sv;",
                               1);
        connectNodes(metadata, read, 1, true);
        NodePtr move = transform(read);
        drive(move, kGeoTransformParamTranslate, 1, "loop.iteration", 0., 100.);
        drive(move, kGeoTransformParamTranslate, 2, "loop.ivalue", 0., 1.);
        NodePtr loop = end(move, eGeoForEachIteratePieces, eGeoForEachGatherMerge);
        setChoice(loop, kGeoForEachEndParamPieceMethod, (int)eGeoPieceMethodAttribute);
        setString(loop, kGeoForEachEndParamPieceAttribute, byId ? "id" : "name");
        setString(loop, kGeoForEachEndParamStopCondition,
                  byId ? "loop.ivalue != [-2, 3, 7][loop.iteration] or loop.svalue != ''"
                       : "loop.svalue != ['a', 'b', 'c'][loop.iteration] or loop.ivalue != loop.iteration");

        GeoDetailConstPtr result;
        std::string error;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error)) << error;
        ASSERT_TRUE(result != NULL);
        ASSERT_EQ(18, result->primitiveCount());
        const GeoAttribute* gotIteration = result->findAttribute(eGeoAttribClassPrimitive, "gotIteration");
        const GeoAttribute* gotIvalue = result->findAttribute(eGeoAttribClassPrimitive, "gotIvalue");
        const GeoAttribute* gotSvalue = result->findAttribute(eGeoAttribClassPrimitive, "gotSvalue");
        const GeoAttribute* name = result->findAttribute(eGeoAttribClassPrimitive, "name");
        const GeoAttribute* id = result->findAttribute(eGeoAttribClassPrimitive, "id");
        ASSERT_TRUE(gotIteration != NULL);
        ASSERT_TRUE(gotIvalue != NULL);
        ASSERT_TRUE(gotSvalue != NULL);
        ASSERT_TRUE(name != NULL);
        ASSERT_TRUE(id != NULL);
        const char* const sortedNames[] = { "a", "b", "c" };
        const int sortedIds[] = { -2, 3, 7 };
        for (std::size_t prim = 0; prim < 18; ++prim) {
            const int k = (int)(prim / 6);
            const int expectedIvalue = byId ? sortedIds[k] : k;
            EXPECT_EQ((std::int64_t)k, intAt(*gotIteration, prim)) << "primitive " << prim;
            EXPECT_EQ((std::int64_t)expectedIvalue, intAt(*gotIvalue, prim)) << "primitive " << prim;
            EXPECT_EQ(std::string(byId ? "" : sortedNames[k]), std::string(gotSvalue->getString(prim))) << "primitive " << prim;
            EXPECT_EQ(std::string(sortedNames[k]), std::string(name->getString(prim))) << "primitive " << prim;
            EXPECT_EQ((std::int64_t)sortedIds[k], intAt(*id, prim)) << "primitive " << prim;
        }

        const std::vector<float> after = positions(*result);
        ASSERT_EQ(cube.size() * 3, after.size());
        for (int k = 0; k < 3; ++k) {
            const std::vector<float> box(after.begin() + (std::ptrdiff_t)(k * cube.size()), after.begin() + (std::ptrdiff_t)((k + 1) * cube.size()));
            double b[3];
            centroid(box, b);
            const int expectedIvalue = byId ? sortedIds[k] : k;
            EXPECT_NEAR(c[1] + 100. * k, b[1], 1e-4) << "piece " << k;
            EXPECT_NEAR(c[2] + (double)expectedIvalue, b[2], 1e-4) << "piece " << k;
        }
        EXPECT_EQ(pointsPerBox * 3, (std::size_t)result->pointCount());
    }
}

TEST_F(GeoForEachTest, AMissingPieceAttributeIsAnErrorNamingIt)
{
    NodePtr piece = begin(threeBoxes(), eGeoForEachBeginExtractPiece);
    NodePtr loop = end(piece, eGeoForEachIteratePieces, eGeoForEachGatherMerge);
    setChoice(loop, kGeoForEachEndParamPieceMethod, (int)eGeoPieceMethodAttribute);
    setString(loop, kGeoForEachEndParamPieceAttribute, "doesNotExist");

    GeoDetailConstPtr result;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error));
    QString message;
    int type = 0;
    loop->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8("doesNotExist"))) << message.toStdString();
    EXPECT_EQ((int)eMessageTypeError, type);
}

TEST_F(GeoForEachTest, ZeroPiecesGiveEmptyGeometryAndAnInfoMessage)
{
    NodePtr nothing = wrangle(NodePtr(), "int unused = 0;", 3);
    NodePtr piece = begin(nothing, eGeoForEachBeginExtractPiece);
    NodePtr loop = end(piece, eGeoForEachIteratePieces, eGeoForEachGatherMerge);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    EXPECT_EQ(0, result->pointCount());
    EXPECT_EQ(0, result->primitiveCount());
    QString message;
    int type = -1;
    loop->getPersistentMessage(&message, &type, false);
    EXPECT_FALSE(message.isEmpty());
    EXPECT_EQ((int)eMessageTypeInfo, type);
}

TEST_F(GeoForEachTest, MetadataDetailMatchesTheLoopVariables)
{
    NodePtr input = begin(_cube, eGeoForEachBeginFetchInput);
    NodePtr metadata = begin(_cube, eGeoForEachBeginFetchMetadata);
    NodePtr read = wrangle(input, "int it = detail(1, \"iteration\"); @P.x += 10 * it;", 0);
    connectNodes(metadata, read, 1, true);
    NodePtr loop = end(read, eGeoForEachIterateCount, eGeoForEachGatherMerge);
    setInt(loop, kGeoForEachEndParamIterations, 4);

    NodePtr driven = transform(begin(_cube, eGeoForEachBeginFetchInput));
    drive(driven, kGeoTransformParamTranslate, 0, "loop.iteration", 0., 10.);
    NodePtr drivenLoop = end(driven, eGeoForEachIterateCount, eGeoForEachGatherMerge);
    setInt(drivenLoop, kGeoForEachEndParamIterations, 4);

    GeoDetailConstPtr fromDetail;
    GeoDetailConstPtr fromVariable;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &fromDetail, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(drivenLoop, &fromVariable, &error)) << error;
    ASSERT_TRUE(fromDetail != NULL);
    ASSERT_TRUE(fromVariable != NULL);
    const std::vector<float> a = positions(*fromDetail);
    const std::vector<float> b = positions(*fromVariable);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_NEAR(a[i], b[i], 1e-5) << "component " << i;
    }
    expectOffsetCopies(cubePositions(), a, 4, 10.);
}

TEST_F(GeoForEachTest, MetadataDetailHoldsEveryVariable)
{
    const GeoDetail detail = GeoForEachBegin::metadataDetail(2, 5, 2.5, 3, "b");
    const char* const names[] = { "iteration", "numiterations", "value", "ivalue", "svalue" };
    for (const char* name : names) {
        EXPECT_TRUE(detail.findAttribute(eGeoAttribClassDetail, name) != NULL) << name;
    }
    const GeoAttribute* value = detail.findAttribute(eGeoAttribClassDetail, "value");
    ASSERT_TRUE(value != NULL);
    EXPECT_DOUBLE_EQ(2.5, value->get<double>(0));
    const GeoAttribute* svalue = detail.findAttribute(eGeoAttribClassDetail, "svalue");
    ASSERT_TRUE(svalue != NULL);
    EXPECT_EQ(std::string("b"), std::string(svalue->getString(0)));
    EXPECT_EQ(0, detail.pointCount());
}

// An outer Count loop of three iterations around an inner one of two. The inner body moves the
// cube by 10 along x per outer iteration, read as loops.outer.iteration, and by 100 along y per
// inner iteration, read as loop.iteration. The output is the six copies, outer iteration first.
TEST_F(GeoForEachTest, NestedLoopsReadBothIterations)
{
    NodePtr outerBegin = begin(_cube, eGeoForEachBeginFetchInput);
    NodePtr innerBegin = begin(outerBegin, eGeoForEachBeginFetchInput);
    NodePtr move = transform(innerBegin);
    drive(move, kGeoTransformParamTranslate, 0, "loops.outer.iteration", 0., 10.);
    drive(move, kGeoTransformParamTranslate, 1, "loop.iteration", 0., 100.);
    NodePtr inner = end(move, eGeoForEachIterateCount, eGeoForEachGatherMerge);
    setString(inner, kGeoForEachEndParamLoopName, "inner");
    setInt(inner, kGeoForEachEndParamIterations, 2);
    NodePtr outer = end(inner, eGeoForEachIterateCount, eGeoForEachGatherMerge);
    setString(outer, kGeoForEachEndParamLoopName, "outer");
    setInt(outer, kGeoForEachEndParamIterations, 3);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(outer, &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    const std::vector<float> cube = cubePositions();
    ASSERT_FALSE(cube.empty());
    const std::vector<float> after = positions(*result);
    ASSERT_EQ(cube.size() * 6, after.size());
    for (int o = 0; o < 3; ++o) {
        for (int i = 0; i < 2; ++i) {
            const std::size_t copy = (std::size_t)(2 * o + i);
            for (std::size_t p = 0; p < cube.size(); p += 3) {
                const std::size_t j = copy * cube.size() + p;
                EXPECT_NEAR(cube[p] + 10. * o, after[j], 1e-5) << "outer " << o << " inner " << i << " point " << p / 3;
                EXPECT_NEAR(cube[p + 1] + 100. * i, after[j + 1], 1e-5) << "outer " << o << " inner " << i << " point " << p / 3;
                EXPECT_NEAR(cube[p + 2], after[j + 2], 1e-5) << "outer " << o << " inner " << i << " point " << p / 3;
            }
        }
    }
}

TEST_F(GeoForEachTest, PiecesBench)
{
    // A cube copied onto every point of a grid: one connectivity piece per point.
    const int side = fullBench() ? 100 : 10;
    NodePtr grid = add(PLUGINID_NATRON_GEOPRIMITIVE);
    ASSERT_TRUE(grid != NULL);
    setChoice(grid, kGeoPrimitiveParamType, (int)eGeoShapeGrid);
    setDouble(grid, kGeoPrimitiveParamGridSize, 10. * side, 0);
    setDouble(grid, kGeoPrimitiveParamGridSize, 10. * side, 1);
    setInt(grid, kGeoPrimitiveParamGridRows, side - 1);
    setInt(grid, kGeoPrimitiveParamGridColumns, side - 1);
    NodePtr field = add(PLUGINID_NATRON_GEOCOPYTOPOINTS);
    ASSERT_TRUE(field != NULL);
    connectNodes(_cube, field, 0, true);
    connectNodes(grid, field, 1, true);
    {
        GeoDetailConstPtr points;
        std::string error;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(field, &points, &error)) << error;
    }

    const int pieces = side * side;
    const int allThreads = std::max(1, (int)std::thread::hardware_concurrency());
    const int threadCounts[] = { 1, allThreads };
    for (int threads : threadCounts) {
        // A fresh loop per run, so only the field upstream is served from the cache.
        NodePtr piece = begin(field, eGeoForEachBeginExtractPiece);
        NodePtr move = transform(piece);
        drive(move, kGeoTransformParamTranslate, 1, "loop.t", 0., 10.);
        drive(move, kGeoTransformParamRotate, 1, "loop.t", 0., 360.);
        NodePtr loop = end(move, eGeoForEachIteratePieces, eGeoForEachGatherMerge);
        setChoice(loop, kGeoForEachEndParamPieceMethod, (int)eGeoPieceMethodConnectivity);

        AppTLS::ThreadBudgetScope budget(threads);
        GeoDetailConstPtr result;
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error)) << error;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ASSERT_TRUE(result != NULL);
        EXPECT_EQ((GeoIndex)pieces * 6, result->primitiveCount());
        std::printf("[ foreach  ] pieces %d threads %d: %.1f ms\n", pieces, threads, ms);
        std::fflush(stdout);
    }
}
