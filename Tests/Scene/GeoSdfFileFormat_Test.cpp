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
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Geometry/GeoSdfData.h"
#include "Engine/Nodes/Geometry/GeoSdfFileFormat.h"

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {

void
addQuad(GeoDetailBuilder& b,
        double x)
{
    const GeoOffset first = b.appendPoints(4);
    b.setPointPosition(first, x, 0., 0.);
    b.setPointPosition(first + 1, x + 1., 0., 0.);
    b.setPointPosition(first + 2, x + 1., 1., 0.);
    b.setPointPosition(first + 3, x, 1., 0.);
    const GeoOffset corners[4] = { first, first + 1, first + 2, first + 3 };
    b.appendPrimitive(eGeoPrimitivePolygon, corners);
}

// A quad at /geo/a (two with @p secondQuadInA) and one at /geo/b; @p lift moves /geo/a's first point up.
GeoDetailConstPtr
makeDetail(double lift,
           bool secondQuadInA)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    addQuad(b, 5.);
    if (secondQuadInA) {
        addQuad(b, 10.);
    }
    b.setPointPosition(0, 0., 0., lift);
    GeoAttributeDesc pathDesc;
    pathDesc.name = "path";
    pathDesc.storage = eGeoStorageString;
    GeoAttribute& path = b.addAttribute(eGeoAttribClassPrimitive, pathDesc);
    path.setString(0, "/geo/a");
    path.setString(1, "/geo/b");
    if (secondQuadInA) {
        path.setString(2, "/geo/a");
    }

    return std::make_shared<const GeoDetail>(b.finish());
}

// Serves a detail the test swaps, under a key unique to the test.
class Served {
public:
    explicit Served(const std::string& key)
        : _key(key)
        , _current(std::make_shared<GeoDetailConstPtr>(makeDetail(0., false)))
    {
        std::string error;
        EXPECT_TRUE(GeoLayers::registerPlugin(&error)) << error;
        const std::shared_ptr<GeoDetailConstPtr> current = _current;
        GeoLayers::setProvider(_key, [current]() { return *current; });
    }

    ~Served()
    {
        GeoLayers::removeProvider(_key);
    }

    void set(GeoDetailConstPtr detail)
    {
        *_current = std::move(detail);
    }

    std::string identifier() const
    {
        return GeoLayers::identifier(_key);
    }

private:
    std::string _key;
    std::shared_ptr<GeoDetailConstPtr> _current;
};

// Records the ObjectsChanged notices one stage sends.
class NoticeLog
    : public TfWeakBase {
public:
    explicit NoticeLog(const UsdStageRefPtr& stage)
    {
        _key = TfNotice::Register(TfCreateWeakPtr(this), &NoticeLog::onObjectsChanged, UsdStagePtr(stage));
    }

    ~NoticeLog()
    {
        TfNotice::Revoke(_key);
    }

    void onObjectsChanged(const UsdNotice::ObjectsChanged& notice)
    {
        for (const SdfPath& path : notice.GetResyncedPaths()) {
            resynced.push_back(path);
        }
        for (const SdfPath& path : notice.GetChangedInfoOnlyPaths()) {
            infoOnly.push_back(path);
        }
    }

    bool changedInfo(const SdfPath& path) const
    {
        return std::find(infoOnly.begin(), infoOnly.end(), path) != infoOnly.end();
    }

    std::vector<SdfPath> resynced;
    std::vector<SdfPath> infoOnly;

private:
    TfNotice::Key _key;
};

int
partitionAt(const GeoUsdConvert& convert,
            const SdfPath& path)
{
    for (std::size_t i = 0; i < convert.partitionCount(); ++i) {
        if (convert.partitionPath(i) == path) {
            return int(i);
        }
    }

    return -1;
}

VtVec3fArray
meshPoints(const UsdStageRefPtr& stage,
           const char* path)
{
    VtVec3fArray points;
    UsdGeomMesh(stage->GetPrimAtPath(SdfPath(path))).GetPointsAttr().Get(&points);

    return points;
}

} // namespace

TEST(GeoSdfFileFormat, OpensProviderAndBuildsOnlyReadPartitions)
{
    Served served("geoSdfFileFormatTest_open");
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(served.identifier());
    ASSERT_TRUE(layer);
    EXPECT_EQ(layer->GetIdentifier(), served.identifier());

    const std::shared_ptr<const GeoUsdConvert> convert = geoLayerConvert(layer);
    ASSERT_TRUE(convert);
    const int a = partitionAt(*convert, SdfPath("/geo/a"));
    const int b = partitionAt(*convert, SdfPath("/geo/b"));
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    EXPECT_FALSE(convert->isPartitionBuilt(std::size_t(a)));
    EXPECT_FALSE(convert->isPartitionBuilt(std::size_t(b)));

    const VtValue points = layer->GetField(SdfPath("/geo/a.points"), SdfFieldKeys->Default);
    ASSERT_TRUE(points.IsHolding<VtVec3fArray>());
    EXPECT_EQ(points.UncheckedGet<VtVec3fArray>().size(), 4u);
    EXPECT_TRUE(convert->isPartitionBuilt(std::size_t(a)));
    EXPECT_FALSE(convert->isPartitionBuilt(std::size_t(b)));
}

TEST(GeoSdfFileFormat, StageMeshMatchesDetail)
{
    Served served("geoSdfFileFormatTest_stage");
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(served.identifier());
    ASSERT_TRUE(layer);
    const UsdStageRefPtr stage = UsdStage::Open(layer);
    ASSERT_TRUE(stage);

    const UsdGeomMesh mesh(stage->GetPrimAtPath(SdfPath("/geo/a")));
    ASSERT_TRUE(mesh);
    const VtVec3fArray points = meshPoints(stage, "/geo/a");
    ASSERT_EQ(points.size(), 4u);
    EXPECT_EQ(points[0], GfVec3f(0.f, 0.f, 0.f));
    EXPECT_EQ(points[2], GfVec3f(1.f, 1.f, 0.f));
    VtIntArray counts;
    VtIntArray indices;
    mesh.GetFaceVertexCountsAttr().Get(&counts);
    mesh.GetFaceVertexIndicesAttr().Get(&indices);
    EXPECT_EQ(counts, VtIntArray({ 4 }));
    EXPECT_EQ(indices, VtIntArray({ 0, 1, 2, 3 }));
    EXPECT_EQ(meshPoints(stage, "/geo/b")[0], GfVec3f(5.f, 0.f, 0.f));
}

TEST(GeoSdfFileFormat, PointEditIsAnInfoOnlyChange)
{
    Served served("geoSdfFileFormatTest_points");
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(served.identifier());
    ASSERT_TRUE(layer);
    const UsdStageRefPtr stage = UsdStage::Open(layer);
    ASSERT_TRUE(stage);
    ASSERT_EQ(meshPoints(stage, "/geo/a").size(), 4u);

    NoticeLog log(stage);
    served.set(makeDetail(2., false));
    std::string error;
    ASSERT_TRUE(GeoLayers::refresh(served.identifier(), &error)) << error;

    const SdfLayerHandle found = SdfLayer::Find(served.identifier());
    EXPECT_EQ(get_pointer(found), get_pointer(layer));
    EXPECT_EQ(layer->GetIdentifier(), served.identifier());
    EXPECT_TRUE(log.resynced.empty());
    EXPECT_TRUE(log.changedInfo(SdfPath("/geo/a.points")));
    for (const SdfPath& path : log.infoOnly) {
        EXPECT_FALSE(path.HasPrefix(SdfPath("/geo/b"))) << path.GetString();
    }
    EXPECT_EQ(meshPoints(stage, "/geo/a")[0], GfVec3f(0.f, 0.f, 2.f));
    EXPECT_EQ(meshPoints(stage, "/geo/b")[0], GfVec3f(5.f, 0.f, 0.f));
}

TEST(GeoSdfFileFormat, TopologyEditResyncsOnlyItsPrim)
{
    Served served("geoSdfFileFormatTest_topology");
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(served.identifier());
    ASSERT_TRUE(layer);
    const UsdStageRefPtr stage = UsdStage::Open(layer);
    ASSERT_TRUE(stage);

    NoticeLog log(stage);
    served.set(makeDetail(0., true));
    std::string error;
    ASSERT_TRUE(GeoLayers::refresh(served.identifier(), &error)) << error;

    EXPECT_FALSE(log.resynced.empty());
    for (const SdfPath& path : log.resynced) {
        EXPECT_TRUE(path.HasPrefix(SdfPath("/geo/a"))) << path.GetString();
    }
    for (const SdfPath& path : log.infoOnly) {
        EXPECT_FALSE(path.HasPrefix(SdfPath("/geo/b"))) << path.GetString();
    }
    VtIntArray counts;
    UsdGeomMesh(stage->GetPrimAtPath(SdfPath("/geo/a"))).GetFaceVertexCountsAttr().Get(&counts);
    EXPECT_EQ(counts, VtIntArray({ 4, 4 }));
    EXPECT_EQ(meshPoints(stage, "/geo/a").size(), 8u);
    EXPECT_EQ(meshPoints(stage, "/geo/b").size(), 4u);
}

TEST(GeoSdfFileFormat, SaveFails)
{
    Served served("geoSdfFileFormatTest_save");
    const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(served.identifier());
    ASSERT_TRUE(layer);
    TfErrorMark mark;
    EXPECT_FALSE(layer->Save(true));
    mark.Clear();
}
