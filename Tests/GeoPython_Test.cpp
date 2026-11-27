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

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoPage.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoPython.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const std::size_t kPointCount = 3000;

/** @brief Points along X; z is 1 on the first page and 0 after it; an int `id` per point. */
GeoDetail
makePoints()
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(GeoIndex(kPointCount));
    std::vector<std::int32_t> ids(kPointCount);
    for (std::size_t i = 0; i < kPointCount; ++i) {
        b.setPointPosition(first + GeoOffset(i), double(i), 0., i < kGeoPageSize ? 1. : 0.);
        ids[i] = std::int32_t(i);
    }
    GeoAttributeDesc id;
    id.name = "id";
    id.storage = eGeoStorageInt32;
    b.addAttribute(eGeoAttribClassPoint, id).scatterDense<std::int32_t>(0, kPointCount, std::span<const std::int32_t>(ids));

    return b.finish();
}

GeoDetail
makeGrid()
{
    GeoShapeParams params;
    params.shape = eGeoShapeGrid;
    params.gridRows = 4;
    params.gridColumns = 4;
    GeoDetailBuilder b;
    buildGeoShape(params, b);

    return b.finish();
}

struct RunOutcome {
    GeoPythonRunResultEnum result = eGeoPythonRunFailed;
    std::string error;
    GeoDetail detail;
};

RunOutcome
run(const GeoDetail& input,
    const std::string& script,
    const std::function<bool()>& aborted = std::function<bool()>(),
    double frame = 1.)
{
    RunOutcome out;
    GeoDetailBuilder b(input);
    out.result = GeoPython::runScript(b, script, "GeoPythonTest", frame, aborted, &out.error);
    if (out.result == eGeoPythonRunOK) {
        out.detail = b.finish();
    }

    return out;
}

void
expectPagesShared(const GeoAttribute& x,
                  const GeoAttribute& y,
                  std::size_t firstPage,
                  const char* name)
{
    ASSERT_EQ(x.pageCount(), y.pageCount());
    for (std::size_t i = firstPage; i < x.pageCount(); ++i) {
        EXPECT_EQ(x.values().page(i).get(), y.values().page(i).get()) << name << " page " << i;
    }
}

} // namespace

TEST(GeoPythonTest, DoublingZDoublesItAndKeepsUntouchedPagesShared)
{
    const GeoDetail input = makePoints();
    const RunOutcome out = run(input,
                               "P = geo.points['P']\n"
                               "for i in range(len(P)):\n"
                               "    x, y, z = P[i]\n"
                               "    P[i] = (x, y, z * 2)\n");
    ASSERT_EQ(eGeoPythonRunOK, out.result) << out.error;
    ASSERT_EQ(GeoIndex(kPointCount), out.detail.pointCount());
    for (std::size_t i = 0; i < kPointCount; i += 97) {
        const std::array<double, 3> p = out.detail.pointPosition(GeoOffset(i));
        EXPECT_DOUBLE_EQ(double(i), p[0]);
        EXPECT_DOUBLE_EQ(i < kGeoPageSize ? 2. : 0., p[2]) << "point " << i;
    }

    const GeoAttribute* inP = input.findAttribute(eGeoAttribClassPoint, "P");
    const GeoAttribute* outP = out.detail.findAttribute(eGeoAttribClassPoint, "P");
    ASSERT_NE(nullptr, inP);
    ASSERT_NE(nullptr, outP);
    ASSERT_EQ(std::size_t(3), outP->pageCount());
    EXPECT_NE(inP->values().page(0).get(), outP->values().page(0).get());
    expectPagesShared(*inP, *outP, 1, "P");
    expectPagesShared(*input.findAttribute(eGeoAttribClassPoint, "id"), *out.detail.findAttribute(eGeoAttribClassPoint, "id"), 0, "id");
}

TEST(GeoPythonTest, ASyntaxErrorFailsWithItsLine)
{
    const RunOutcome out = run(makePoints(), "a = 1\nb = = 2\n");
    EXPECT_EQ(eGeoPythonRunFailed, out.result);
    EXPECT_NE(std::string::npos, out.error.find("SyntaxError")) << out.error;
    EXPECT_NE(std::string::npos, out.error.find("line 2")) << out.error;
}

TEST(GeoPythonTest, AWrongLengthWriteFailsWithItsLine)
{
    const RunOutcome out = run(makePoints(), "a = 1\ngeo.points['P'] = [(0, 0, 0)]\n");
    EXPECT_EQ(eGeoPythonRunFailed, out.result);
    EXPECT_NE(std::string::npos, out.error.find("ValueError")) << out.error;
    EXPECT_NE(std::string::npos, out.error.find("line 2")) << out.error;
    EXPECT_NE(std::string::npos, out.error.find("geo.points['P']")) << out.error;
}

TEST(GeoPythonTest, AnExceptionReportsTheTraceback)
{
    const RunOutcome out = run(makePoints(),
                               "def f():\n"
                               "    raise RuntimeError('boom')\n"
                               "f()\n");
    EXPECT_EQ(eGeoPythonRunFailed, out.result);
    EXPECT_NE(std::string::npos, out.error.find("Traceback")) << out.error;
    EXPECT_NE(std::string::npos, out.error.find("RuntimeError: boom")) << out.error;
    EXPECT_NE(std::string::npos, out.error.find("line 2")) << out.error;
    EXPECT_NE(std::string::npos, out.error.find("line 3")) << out.error;
}

TEST(GeoPythonTest, AssigningANewNameCreatesTheAttribute)
{
    const RunOutcome out = run(makeGrid(),
                               "n = len(geo.points)\n"
                               "geo.points['Cd'] = [(1.0, 0.5, 0.25)] * n\n"
                               "geo.points['mask'] = 3\n"
                               "geo.prims['name'] = 'piece'\n"
                               "geo.detail['scale'] = 2.5\n");
    ASSERT_EQ(eGeoPythonRunOK, out.result) << out.error;
    const GeoAttribute* cd = out.detail.findAttribute(eGeoAttribClassPoint, "Cd");
    ASSERT_NE(nullptr, cd);
    EXPECT_EQ(3, cd->tupleSize());
    EXPECT_EQ(eGeoStorageFloat32, cd->storage());
    EXPECT_EQ(eGeoTypeInfoColour, cd->desc().typeInfo);
    std::vector<float> values(std::size_t(out.detail.pointCount()) * 3);
    cd->copyDense<float>(0, std::size_t(out.detail.pointCount()), std::span<float>(values));
    EXPECT_FLOAT_EQ(0.5f, values[1]);
    EXPECT_FLOAT_EQ(0.25f, values[values.size() - 1]);

    const GeoAttribute* mask = out.detail.findAttribute(eGeoAttribClassPoint, "mask");
    ASSERT_NE(nullptr, mask);
    EXPECT_EQ(eGeoStorageInt32, mask->storage());
    std::vector<std::int32_t> masks(std::size_t(out.detail.pointCount()));
    mask->copyDense<std::int32_t>(0, masks.size(), std::span<std::int32_t>(masks));
    EXPECT_EQ(3, masks.back());

    const GeoAttribute* name = out.detail.findAttribute(eGeoAttribClassPrimitive, "name");
    ASSERT_NE(nullptr, name);
    EXPECT_EQ("piece", name->getString(0));

    const GeoAttribute* scale = out.detail.findAttribute(eGeoAttribClassDetail, "scale");
    ASSERT_NE(nullptr, scale);
    float s = 0.f;
    scale->copyDense<float>(0, 1, std::span<float>(&s, 1));
    EXPECT_FLOAT_EQ(2.5f, s);
}

TEST(GeoPythonTest, ATombstonedInputReadsDenselyInNumberOrder)
{
    GeoDetailBuilder b(makePoints());
    const std::vector<GeoOffset> doomed = { 0, 1, 2, 10 };
    b.deletePoints(doomed);
    const GeoDetail input = b.finish();
    ASSERT_EQ(GeoIndex(kPointCount - 4), input.pointCount());

    const RunOutcome out = run(input,
                               "ids = geo.points['id']\n"
                               "assert len(ids) == len(geo.points)\n"
                               "assert ids[0] == 3, ids[0]\n"
                               "assert ids[7] == 11, ids[7]\n"
                               "geo.points['id'] = [v * 10 for v in ids]\n");
    ASSERT_EQ(eGeoPythonRunOK, out.result) << out.error;
    const GeoAttribute* id = out.detail.findAttribute(eGeoAttribClassPoint, "id");
    ASSERT_NE(nullptr, id);
    std::vector<std::int32_t> ids(1);
    id->copyDense<std::int32_t>(std::size_t(out.detail.pointMap().offsetFromIndex(0)),
                                std::size_t(out.detail.pointMap().offsetFromIndex(0)) + 1,
                                std::span<std::int32_t>(ids));
    EXPECT_EQ(30, ids[0]);
}

TEST(GeoPythonTest, GroupsReadAndWriteAsMasks)
{
    const RunOutcome out = run(makeGrid(),
                               "P = geo.points['P']\n"
                               "geo.points.set_group('left', [p[0] < 0 for p in P])\n"
                               "assert 'left' in geo.points.groups()\n"
                               "g = geo.points.group('left')\n"
                               "assert sum(1 for v in g if v) > 0\n");
    ASSERT_EQ(eGeoPythonRunOK, out.result) << out.error;
    const GeoElementGroup* left = out.detail.findGroup(eGeoAttribClassPoint, "left");
    ASSERT_NE(nullptr, left);
    out.detail.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset p) {
        EXPECT_EQ(out.detail.pointPosition(p)[0] < 0., left->contains(p)) << "point " << p;
    });
}

TEST(GeoPythonTest, AddedPointsAndPrimitivesAppearAfterTheScript)
{
    const GeoDetail input = makeGrid();
    const RunOutcome out = run(input,
                               "n = len(geo.points)\n"
                               "pts = geo.add_points([(0, 0, 5), (1, 0, 5), (0, 1, 5)])\n"
                               "assert len(geo.points) == n\n"
                               "prim = geo.add_prim(list(pts))\n"
                               "assert prim == len(geo.prims)\n"
                               "geo.remove_prims([0])\n");
    ASSERT_EQ(eGeoPythonRunOK, out.result) << out.error;
    EXPECT_EQ(input.pointCount() + 3, out.detail.pointCount());
    EXPECT_EQ(input.primitiveCount(), out.detail.primitiveCount());
    const GeoOffset last = out.detail.primitiveMap().offsetFromIndex(out.detail.primitiveCount() - 1);
    EXPECT_EQ(3, out.detail.primitiveVertices(last).count);
    const GeoOffset point = out.detail.vertexPoint(out.detail.primitiveVertices(last).start);
    EXPECT_DOUBLE_EQ(5., out.detail.pointPosition(point)[2]);
}

TEST(GeoPythonTest, RemovingPointsRemovesThePrimitivesUsingThem)
{
    const GeoDetail input = makeGrid();
    const RunOutcome out = run(input, "geo.remove_points([0])\n");
    ASSERT_EQ(eGeoPythonRunOK, out.result) << out.error;
    EXPECT_EQ(input.pointCount() - 1, out.detail.pointCount());
    EXPECT_EQ(input.primitiveCount() - 1, out.detail.primitiveCount());
}

TEST(GeoPythonTest, AnAbortStopsTheScript)
{
    const RunOutcome out = run(makePoints(),
                               "while True:\n"
                               "    try:\n"
                               "        pass\n"
                               "    except BaseException:\n"
                               "        pass\n",
                               [] { return true; });
    EXPECT_EQ(eGeoPythonRunAborted, out.result) << out.error;
}

TEST(GeoPythonTest, GeoKeptPastTheScriptStopsWorking)
{
    const RunOutcome first = run(makePoints(),
                                 "import builtins\n"
                                 "builtins._kept_geo = geo\n"
                                 "builtins._kept_P = geo.points['P']\n");
    ASSERT_EQ(eGeoPythonRunOK, first.result) << first.error;
    const RunOutcome second = run(makePoints(),
                                  "import builtins\n"
                                  "kept = builtins._kept_geo\n"
                                  "assert len(builtins._kept_P) == 3000\n"
                                  "del builtins._kept_geo, builtins._kept_P\n"
                                  "kept.points['P']\n");
    EXPECT_EQ(eGeoPythonRunFailed, second.result);
    EXPECT_NE(std::string::npos, second.error.find("RuntimeError")) << second.error;
    EXPECT_NE(std::string::npos, second.error.find("line 5")) << second.error;
}

TEST(GeoPythonTest, AFailingScriptOnAnotherThreadReportsAndReturns)
{
    const GeoDetail input = makePoints();
    RunOutcome outcomes[2];
    std::thread worker([&] {
        outcomes[0] = run(input, "geo.points['P'] = [1, 2]\n");
        outcomes[1] = run(input, "geo.points['P'] = 0.0\n");
    });
    worker.join();
    EXPECT_EQ(eGeoPythonRunFailed, outcomes[0].result);
    EXPECT_NE(std::string::npos, outcomes[0].error.find("ValueError")) << outcomes[0].error;
    ASSERT_EQ(eGeoPythonRunOK, outcomes[1].result) << outcomes[1].error;
    EXPECT_DOUBLE_EQ(0., outcomes[1].detail.pointPosition(GeoOffset(5))[0]);
}

class GeoPythonNodeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _source = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOPRIMITIVE));
        _python = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOPYTHON));
        ASSERT_TRUE(_source != NULL);
        ASSERT_TRUE(_python != NULL);
        connectNodes(_source, _python, 0, true);
    }

    virtual void TearDown() OVERRIDE
    {
        NodePtr* nodes[] = { &_python, &_source };
        for (NodePtr* node : nodes) {
            if (*node) {
                appPTR->removeAllCacheEntriesForHolder(node->get(), true);
                (*node)->destroyNode(false, false);
                node->reset();
            }
        }
        BaseTest::TearDown();
    }

    void setScript(const std::string& script)
    {
        const KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(_python->getKnobByName(kGeoPythonParamScript));
        ASSERT_TRUE(knob != NULL);
        knob->setValue(script);
    }

    EffectInstance::RenderRoIRetCode evaluate(GeoDetailConstPtr* detail,
                                              std::string* error)
    {
        return _python->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0), true /*byPassCache*/), detail, error);
    }

    NodePtr _source;
    NodePtr _python;
};

TEST_F(GeoPythonNodeTest, TheScriptEditsTheInput)
{
    setScript("P = geo.points['P']\n"
              "geo.points['P'] = [(x, y, z + 1.0) for x, y, z in P]\n");
    GeoDetailConstPtr detail;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&detail, &error)) << error;
    ASSERT_TRUE(detail != NULL);

    GeoDetailConstPtr source;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              _source->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0), true /*byPassCache*/), &source, &error))
        << error;
    ASSERT_TRUE(source != NULL);
    ASSERT_GT(detail->pointCount(), 0);
    ASSERT_EQ(source->pointCount(), detail->pointCount());
    for (GeoIndex i = 0; i < detail->pointCount(); ++i) {
        const std::array<double, 3> before = source->pointPosition(source->pointMap().offsetFromIndex(i));
        const std::array<double, 3> after = detail->pointPosition(detail->pointMap().offsetFromIndex(i));
        EXPECT_DOUBLE_EQ(before[0], after[0]) << "point " << i;
        EXPECT_NEAR(before[2] + 1., after[2], 1e-6) << "point " << i;
    }
}

TEST_F(GeoPythonNodeTest, ErrorsReportOnTheNodeAndTheNextRenderWorks)
{
    GeoDetailConstPtr detail;
    std::string error;

    setScript("a = 1\nb = = 2\n");
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(&detail, &error));
    EXPECT_NE(std::string::npos, error.find("SyntaxError")) << error;
    EXPECT_NE(std::string::npos, error.find("line 2")) << error;

    error.clear();
    detail.reset();
    setScript("a = 1\ngeo.points['P'] = [(0, 0, 0)] * (len(geo.points) + 1)\n");
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(&detail, &error));
    EXPECT_NE(std::string::npos, error.find("ValueError")) << error;
    EXPECT_NE(std::string::npos, error.find("line 2")) << error;

    error.clear();
    detail.reset();
    setScript("geo.detail['ok'] = 1\n");
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&detail, &error)) << error;
    ASSERT_TRUE(detail != NULL);
    EXPECT_NE(nullptr, detail->findAttribute(eGeoAttribClassDetail, "ok"));
}

TEST_F(GeoPythonNodeTest, ABlankScriptPassesTheInputThrough)
{
    setScript("  \n");
    GeoDetailConstPtr detail;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(&detail, &error)) << error;
    ASSERT_TRUE(detail != NULL);

    GeoDetailConstPtr source;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              _source->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0), true /*byPassCache*/), &source, &error))
        << error;
    ASSERT_TRUE(source != NULL);
    EXPECT_EQ(source->pointCount(), detail->pointCount());
    EXPECT_EQ(source->primitiveCount(), detail->primitiveCount());
}

TEST_F(GeoPythonNodeTest, TheHelpPageCarriesTheBundledReference)
{
    const QString page = _python->makeDocumentation(false);
    EXPECT_TRUE(page.contains(QString::fromUtf8("## Element classes"))) << page.toStdString();
    EXPECT_TRUE(page.contains(QString::fromUtf8("geo.add_prims(counts, points")));
    EXPECT_FALSE(page.contains(QString::fromUtf8("Tolerance:")));
}
