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
                                          GeoDetailConstPtr* detail,
                                          std::string* error,
                                          bool byPassCache = false)
    {
        const double time = 1.;
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
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

TEST_F(GeoForEachTest, PiecesByNameAreScaledAboutTheirOwnCentroids)
{
    expectBoxesScaledAboutTheirCentroids(eGeoPieceMethodAttribute);
}

TEST_F(GeoForEachTest, PiecesByConnectivityAreScaledAboutTheirOwnCentroids)
{
    expectBoxesScaledAboutTheirCentroids(eGeoPieceMethodConnectivity);
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

    NodePtr piece = begin(field, eGeoForEachBeginExtractPiece);
    NodePtr move = transform(piece);
    drive(move, kGeoTransformParamTranslate, 1, "loop.t", 0., 10.);
    drive(move, kGeoTransformParamRotate, 1, "loop.t", 0., 360.);
    NodePtr loop = end(move, eGeoForEachIteratePieces, eGeoForEachGatherMerge);
    setChoice(loop, kGeoForEachEndParamPieceMethod, (int)eGeoPieceMethodConnectivity);

    const int pieces = side * side;
    const int allThreads = std::max(1, (int)std::thread::hardware_concurrency());
    const int threadCounts[] = { 1, allThreads };
    for (int threads : threadCounts) {
        AppTLS::ThreadBudgetScope budget(threads);
        GeoDetailConstPtr result;
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(loop, &result, &error, true)) << error;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ASSERT_TRUE(result != NULL);
        EXPECT_EQ((GeoIndex)pieces * 6, result->primitiveCount());
        std::printf("[ foreach  ] pieces %d threads %d: %.1f ms\n", pieces, threads, ms);
        std::fflush(stdout);
    }
}
