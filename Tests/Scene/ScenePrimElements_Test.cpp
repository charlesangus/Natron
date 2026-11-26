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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <atomic>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePrimElements.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kMesh = R"(
def Mesh "Quad"
{
    int[] faceVertexCounts = [3, 3]
    int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]
    point3f[] points.timeSamples = {
        1: [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)],
        2: [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)],
    }
    color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 1)] (
        interpolation = "vertex"
    )
    float2[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 0), (1, 1), (0, 1)] (
        interpolation = "faceVarying"
    )
    float[] primvars:faceWeight = [0.5, 0.25] (
        interpolation = "uniform"
    )
    float primvars:roughness = 0.75 (
        interpolation = "constant"
    )
    float[] primvars:tags = [1, 2, 3, 4, 5] (
        interpolation = "constant"
    )
    int[] primvars:label = [10, 20] (
        interpolation = "vertex"
    )
    int[] primvars:label:indices = [0, 1, 1, 0]
    float[] primvars:pair = [1, 2, 3, 4, 5, 6, 7, 8] (
        elementSize = 2
        interpolation = "vertex"
    )
    float[] primvars:bad = [1, 2, 3] (
        interpolation = "vertex"
    )

    def GeomSubset "top"
    {
        uniform token elementType = "face"
        uniform token familyName = "part"
        int[] indices = [1]
    }

    def GeomSubset "corner"
    {
        uniform token elementType = "point"
        int[] indices = [0, 2]
    }
}

def Xform "Src"
{
    def Mesh "Geo"
    {
        point3f[] points = [(0, 0, 0)]
    }
}

def Xform "Asset" (
    prepend payload = </Src>
)
{
}
)";

ScenePayload
payloadOf(const std::string& body)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ScenePrimElementsTest");

    EXPECT_TRUE(layer->ImportFromString("#usda 1.0\n" + body));

    return SceneOps::appendLayer(ScenePayload(), layer, 1);
}

ScenePrimElementsStageReader
readerOf(const SceneStage& stage,
         int* calls = nullptr)
{
    return [&stage, calls](const std::function<void(const SceneStage*)>& read) {
        if (calls) {
            ++*calls;
        }
        read(&stage);

        return true;
    };
}

int
columnOf(const GeoSpreadsheetSource& source,
         GeoAttribClassEnum cls,
         const std::string& name)
{
    const std::vector<GeoSpreadsheetColumn>& columns = source.columns(cls);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].name == name) {
            return int(i);
        }
    }
    ADD_FAILURE() << "no column " << name;

    return -1;
}

std::vector<std::string>
cellsOf(const GeoSpreadsheetSource& source,
        GeoAttribClassEnum cls,
        const std::string& name)
{
    std::vector<std::string> out;
    const int column = columnOf(source, cls, name);
    if (column >= 0) {
        source.readCells(cls, 0, source.rowCount(cls), std::span<const int>(&column, 1), out);
    }

    return out;
}

typedef std::vector<std::string> Cells;
} // anonymous namespace

TEST(ScenePrimElements, MeshPrimvarsLandInTheirClasses)
{
    SceneStage stage;
    ASSERT_TRUE(stage.isValid());
    stage.sync(payloadOf(kMesh));

    int calls = 0;
    const ScenePrimElements read = SceneOps::readPrimElements(readerOf(stage, &calls), "/Quad", 1., 24.);
    ASSERT_EQ(eScenePrimElementsStatusOk, read.status);
    ASSERT_TRUE(bool(read.source));
    const ScenePrimElementsSource& s = *read.source;
    EXPECT_GT(calls, 10) << "one lock per attribute read";

    EXPECT_EQ(4, s.rowCount(eGeoAttribClassPoint));
    EXPECT_EQ(6, s.rowCount(eGeoAttribClassVertex));
    EXPECT_EQ(2, s.rowCount(eGeoAttribClassPrimitive));
    EXPECT_EQ(1, s.rowCount(eGeoAttribClassDetail));

    EXPECT_EQ(Cells({ "0", "1", "1", "0" }), cellsOf(s, eGeoAttribClassPoint, "points[x]"));
    EXPECT_EQ(Cells({ "0", "1", "0", "1" }), cellsOf(s, eGeoAttribClassPoint, "displayColor[g]"));
    EXPECT_EQ(Cells({ "10", "20", "20", "10" }), cellsOf(s, eGeoAttribClassPoint, "label"));
    EXPECT_EQ(Cells({ "(2) [1, 2]", "(2) [3, 4]", "(2) [5, 6]", "(2) [7, 8]" }), cellsOf(s, eGeoAttribClassPoint, "pair"));
    EXPECT_TRUE(s.columns(eGeoAttribClassPoint)[std::size_t(columnOf(s, eGeoAttribClassPoint, "pair"))].hasFullValue);

    EXPECT_EQ(Cells({ "0", "1", "2", "0", "2", "3" }), cellsOf(s, eGeoAttribClassVertex, "Point"));
    EXPECT_EQ(Cells({ "0", "1", "1", "0", "1", "0" }), cellsOf(s, eGeoAttribClassVertex, "st[0]"));
    EXPECT_EQ(Cells({ "0", "0", "1", "0", "1", "1" }), cellsOf(s, eGeoAttribClassVertex, "st[1]"));

    EXPECT_EQ(Cells({ "0.5", "0.25" }), cellsOf(s, eGeoAttribClassPrimitive, "faceWeight"));

    EXPECT_EQ(Cells({ "0.75" }), cellsOf(s, eGeoAttribClassDetail, "roughness"));
    EXPECT_EQ(Cells({ "(5) [1, 2, 3, \xE2\x80\xA6]" }), cellsOf(s, eGeoAttribClassDetail, "tags"));
    EXPECT_EQ("(5) [1, 2, 3, 4, 5]", s.fullValue(eGeoAttribClassDetail, columnOf(s, eGeoAttribClassDetail, "tags"), 0));

    for (const GeoAttribClassEnum cls : { eGeoAttribClassVertex, eGeoAttribClassPrimitive, eGeoAttribClassDetail }) {
        for (const GeoSpreadsheetColumn& c : s.columns(cls)) {
            EXPECT_NE("displayColor[r]", c.name) << "a vertex primvar shows only on points";
            EXPECT_TRUE(c.problem.empty()) << c.name;
        }
    }
    EXPECT_EQ(2., std::get<double>(s.sortKey(eGeoAttribClassVertex, columnOf(s, eGeoAttribClassVertex, "Point"), 2)));
}

TEST(ScenePrimElements, MismatchedPrimvarIsFlaggedWithEmptyCells)
{
    SceneStage stage;
    stage.sync(payloadOf(kMesh));
    const ScenePrimElements read = SceneOps::readPrimElements(readerOf(stage), "/Quad", 1., 24.);
    ASSERT_TRUE(bool(read.source));
    const ScenePrimElementsSource& s = *read.source;

    const int column = columnOf(s, eGeoAttribClassPoint, "bad");
    ASSERT_GE(column, 0);
    EXPECT_FALSE(s.columns(eGeoAttribClassPoint)[std::size_t(column)].problem.empty());
    EXPECT_EQ(Cells({ "", "", "", "" }), cellsOf(s, eGeoAttribClassPoint, "bad"));
    EXPECT_TRUE(s.columns(eGeoAttribClassPoint)[std::size_t(columnOf(s, eGeoAttribClassPoint, "label"))].problem.empty());
}

TEST(ScenePrimElements, TimeSampledPointsDifferByFrame)
{
    SceneStage stage;
    stage.sync(payloadOf(kMesh));
    const ScenePrimElements first = SceneOps::readPrimElements(readerOf(stage), "/Quad", 1., 24.);
    const ScenePrimElements second = SceneOps::readPrimElements(readerOf(stage), "/Quad", 2., 24.);
    ASSERT_TRUE(bool(first.source));
    ASSERT_TRUE(bool(second.source));

    EXPECT_EQ(Cells({ "0", "0", "0", "0" }), cellsOf(*first.source, eGeoAttribClassPoint, "points[z]"));
    EXPECT_EQ(Cells({ "1", "1", "1", "1" }), cellsOf(*second.source, eGeoAttribClassPoint, "points[z]"));
}

TEST(ScenePrimElements, SubsetsListUnderTheirClasses)
{
    SceneStage stage;
    stage.sync(payloadOf(kMesh));
    const ScenePrimElements read = SceneOps::readPrimElements(readerOf(stage), "/Quad", 1., 24.);
    ASSERT_TRUE(bool(read.source));

    const std::vector<ScenePrimElementGroup>& groups = read.source->groups();
    ASSERT_EQ(2u, groups.size());
    for (const ScenePrimElementGroup& g : groups) {
        if (g.name == "top") {
            EXPECT_EQ(eGeoAttribClassPrimitive, g.cls);
            EXPECT_EQ(PXR_NS::VtIntArray({ 1 }), g.indices);
        } else {
            EXPECT_EQ("corner", g.name);
            EXPECT_EQ(eGeoAttribClassPoint, g.cls);
            EXPECT_EQ(PXR_NS::VtIntArray({ 0, 2 }), g.indices);
        }
    }
}

TEST(ScenePrimElements, UnloadedPayloadIsNeverLoaded)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.isValid());
    stage.sync(payloadOf(kMesh));
    const PXR_NS::SdfPathSet loadSet = stage.getStage()->GetLoadSet();
    const U64 loadEdits = stage.getLoadEditCount();
    ASSERT_FALSE(stage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/Asset")).IsLoaded());

    EXPECT_EQ(eScenePrimElementsStatusNotLoaded, SceneOps::readPrimElements(readerOf(stage), "/Asset", 1., 24.).status);
    const ScenePrimElements inside = SceneOps::readPrimElements(readerOf(stage), "/Asset/Geo", 1., 24.);
    EXPECT_EQ(eScenePrimElementsStatusNotLoaded, inside.status);
    EXPECT_FALSE(bool(inside.source));

    EXPECT_EQ(loadSet, stage.getStage()->GetLoadSet());
    EXPECT_EQ(loadEdits, stage.getLoadEditCount());
    EXPECT_FALSE(stage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/Asset")).IsLoaded());
    EXPECT_FALSE(bool(stage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/Asset/Geo"))));
}

TEST(ScenePrimElements, MissingPrimAndRefusedLock)
{
    SceneStage stage;
    stage.sync(payloadOf(kMesh));
    EXPECT_EQ(eScenePrimElementsStatusMissing, SceneOps::readPrimElements(readerOf(stage), "/Nope", 1., 24.).status);
    EXPECT_EQ(eScenePrimElementsStatusMissing, SceneOps::readPrimElements(readerOf(stage), "/Quad/Nope", 1., 24.).status);

    const ScenePrimElementsStageReader refuse = [](const std::function<void(const SceneStage*)>&) { return false; };
    EXPECT_EQ(eScenePrimElementsStatusCancelled, SceneOps::readPrimElements(refuse, "/Quad", 1., 24.).status);

    const std::atomic<bool> cancelled(true);
    EXPECT_EQ(eScenePrimElementsStatusCancelled, SceneOps::readPrimElements(readerOf(stage), "/Quad", 1., 24., &cancelled).status);
}

TEST(ScenePrimElements, ArrayCellsSortByTheirFirstValue)
{
    SceneStage stage;
    stage.sync(payloadOf(kMesh));
    const ScenePrimElements read = SceneOps::readPrimElements(readerOf(stage), "/Quad", 1., 24.);
    ASSERT_TRUE(bool(read.source));
    const ScenePrimElementsSource& s = *read.source;
    const int pair = columnOf(s, eGeoAttribClassPoint, "pair");
    ASSERT_GE(pair, 0);
    for (std::int64_t row = 0; row < 4; ++row) {
        EXPECT_EQ(double(2 * row + 1), std::get<double>(s.sortKey(eGeoAttribClassPoint, pair, row))) << row;
    }
    EXPECT_EQ(1., std::get<double>(s.sortKey(eGeoAttribClassDetail, columnOf(s, eGeoAttribClassDetail, "tags"), 0)));
}

TEST(ScenePrimElements, PointOfVertexReadsTheTopologyNotAPrimvarNamedPoint)
{
    SceneStage stage;
    stage.sync(payloadOf(R"(
def Mesh "Tri"
{
    int[] primvars:Point = [7, 7, 7] (
        interpolation = "faceVarying"
    )
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [2, 0, 1]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)"));
    const ScenePrimElements read = SceneOps::readPrimElements(readerOf(stage), "/Tri", 1., 24.);
    ASSERT_TRUE(bool(read.source));
    const ScenePrimElementsSource& s = *read.source;
    EXPECT_EQ(2, s.pointOfVertex(0));
    EXPECT_EQ(0, s.pointOfVertex(1));
    EXPECT_EQ(1, s.pointOfVertex(2));
    EXPECT_EQ(-1, s.pointOfVertex(3));
}
