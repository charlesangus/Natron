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
#include "Engine/Nodes/Geometry/GeoViewerLayer.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"

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

// Two quads, /geo/a and /geo/b, with the first point of /geo/a lifted by @p lift.
GeoDetailConstPtr
makeDetail(double lift)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    addQuad(b, 5.);
    b.setPointPosition(0, 0., 0., lift);
    GeoAttributeDesc pathDesc;
    pathDesc.name = "path";
    pathDesc.storage = eGeoStorageString;
    GeoAttribute& path = b.addAttribute(eGeoAttribClassPrimitive, pathDesc);
    path.setString(0, "/geo/a");
    path.setString(1, "/geo/b");

    return std::make_shared<const GeoDetail>(b.finish());
}

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

VtVec3fArray
meshPoints(const UsdStageRefPtr& stage,
           const char* path)
{
    VtVec3fArray points;
    UsdGeomMesh(stage->GetPrimAtPath(SdfPath(path))).GetPointsAttr().Get(&points);

    return points;
}

VtVec3fArray
layerPoints(const SdfLayerHandle& layer,
            const char* path)
{
    const VtValue points = layer->GetField(SdfPath(path), SdfFieldKeys->Default);

    return points.IsHolding<VtVec3fArray>() ? points.UncheckedGet<VtVec3fArray>() : VtVec3fArray();
}

} // namespace

TEST(GeoViewerLayer, ConsumerLayerIsSeparateFromSceneLayers)
{
    std::string error;
    ASSERT_TRUE(GeoLayers::registerPlugin(&error)) << error;
    const std::string key = "geoViewerLayerTest_consumer";
    const std::string consumer = "viewer-test";
    const std::shared_ptr<GeoDetailConstPtr> scene = std::make_shared<GeoDetailConstPtr>(makeDetail(0.));
    const std::shared_ptr<GeoDetailConstPtr> viewed = std::make_shared<GeoDetailConstPtr>(makeDetail(0.));
    GeoLayers::setProvider(key, [scene]() { return *scene; });
    GeoLayers::setProvider(key, [viewed]() { return *viewed; }, consumer);

    const std::string sceneIdentifier = GeoLayers::identifier(key);
    const std::string viewerIdentifier = GeoLayers::identifier(key, std::string(), std::string(), consumer);
    EXPECT_NE(sceneIdentifier, viewerIdentifier);
    EXPECT_FALSE(GeoLayers::isConsumerLayer(sceneIdentifier));
    std::string servedTo;
    EXPECT_TRUE(GeoLayers::isConsumerLayer(viewerIdentifier, &servedTo));
    EXPECT_EQ(servedTo, consumer);

    const SdfLayerRefPtr sceneLayer = SdfLayer::FindOrOpen(sceneIdentifier);
    const SdfLayerRefPtr viewerLayer = SdfLayer::FindOrOpen(viewerIdentifier);
    ASSERT_TRUE(static_cast<bool>(sceneLayer));
    ASSERT_TRUE(static_cast<bool>(viewerLayer));
    EXPECT_NE(get_pointer(sceneLayer), get_pointer(viewerLayer));
    EXPECT_NE(geoLayerConvert(sceneLayer), geoLayerConvert(viewerLayer));
    ASSERT_EQ(layerPoints(sceneLayer, "/geo/a.points").size(), 4u);
    ASSERT_EQ(layerPoints(viewerLayer, "/geo/a.points").size(), 4u);

    // Only the viewer's provider moves; refreshing its layer leaves the scene layer as it was.
    *viewed = makeDetail(3.);
    bool edited = false;
    ASSERT_TRUE(GeoLayers::refresh(viewerIdentifier, &error, &edited)) << error;
    EXPECT_TRUE(edited);
    EXPECT_EQ(layerPoints(viewerLayer, "/geo/a.points")[0], GfVec3f(0.f, 0.f, 3.f));
    EXPECT_EQ(layerPoints(sceneLayer, "/geo/a.points")[0], GfVec3f(0.f, 0.f, 0.f));
    EXPECT_EQ(GeoLayers::currentDetail(key), *scene);
    EXPECT_EQ(GeoLayers::currentDetail(key, consumer), *viewed);

    GeoLayers::removeProvider(key, consumer);
    EXPECT_TRUE(GeoLayers::hasProvider(key));
    EXPECT_FALSE(GeoLayers::hasProvider(key, consumer));
    GeoLayers::removeProvider(key);
}

TEST(GeoViewerLayer, ServesOneStackWhateverTheDetail)
{
    GeoViewerLayer viewer("viewer-test-stack");
    ScenePayload first, second;
    std::string error;
    ASSERT_TRUE(viewer.serve(makeDetail(0.), &first, &error)) << error;
    ASSERT_TRUE(viewer.serve(makeDetail(1.), &second, &error)) << error;

    const std::vector<std::string> layers = GeoViewerLayer::findLayers(first);
    ASSERT_EQ(layers.size(), 1u);
    EXPECT_EQ(layers[0], viewer.getIdentifier());
    EXPECT_EQ(GeoViewerLayer::findLayers(second), layers);
    EXPECT_TRUE(GeoLayers::isConsumerLayer(viewer.getIdentifier()));

    SceneStage stage;
    EXPECT_TRUE(stage.sync(first));
    EXPECT_FALSE(stage.sync(second));
}

TEST(GeoViewerLayer, InPlaceRefreshChangesOnlyPointsWithoutResync)
{
    GeoViewerLayer viewer("viewer-test-refresh");
    ScenePayload payload;
    std::string error;
    ASSERT_TRUE(viewer.serve(makeDetail(0.), &payload, &error)) << error;
    SceneStage stage;
    stage.sync(payload);
    const UsdStageRefPtr& usd = stage.getStage();
    ASSERT_EQ(meshPoints(usd, "/geo/a").size(), 4u);
    ASSERT_EQ(meshPoints(usd, "/geo/b").size(), 4u);

    NoticeLog log(usd);
    ASSERT_TRUE(viewer.serve(makeDetail(2.), &payload, &error)) << error;
    EXPECT_FALSE(stage.sync(payload));
    bool edited = false;
    ASSERT_TRUE(GeoViewerLayer::refreshLayers(payload, &edited, &error)) << error;

    EXPECT_TRUE(edited);
    EXPECT_TRUE(log.resynced.empty());
    EXPECT_TRUE(log.changedInfo(SdfPath("/geo/a.points")));
    for (const SdfPath& path : log.infoOnly) {
        EXPECT_FALSE(path.HasPrefix(SdfPath("/geo/b"))) << path.GetString();
        EXPECT_TRUE(path.IsPropertyPath()) << path.GetString();
    }
    EXPECT_EQ(meshPoints(usd, "/geo/a")[0], GfVec3f(0.f, 0.f, 2.f));
    EXPECT_EQ(meshPoints(usd, "/geo/b")[0], GfVec3f(5.f, 0.f, 0.f));

    // Serving the detail the layer already holds edits nothing.
    log.infoOnly.clear();
    ASSERT_TRUE(GeoViewerLayer::refreshLayers(payload, &edited, &error)) << error;
    EXPECT_FALSE(edited);
    EXPECT_TRUE(log.infoOnly.empty());
}

TEST(GeoViewerLayer, TimeVaryingUpstreamUpdatesTheCopyEachFrame)
{
    GeoViewerLayer viewer("viewer-test-frames");
    ScenePayload payload;
    std::string error;
    ASSERT_TRUE(viewer.serve(makeDetail(0.), &payload, &error)) << error;
    SceneStage stage;
    stage.sync(payload);
    const UsdStageRefPtr& usd = stage.getStage();
    const std::string identifier = viewer.getIdentifier();
    const SdfLayerHandle layer = SdfLayer::Find(identifier);
    ASSERT_TRUE(static_cast<bool>(layer));
    ASSERT_EQ(meshPoints(usd, "/geo/a").size(), 4u);

    NoticeLog log(usd);
    for (int frame = 1; frame <= 5; ++frame) {
        // The upstream evaluated at this frame.
        ASSERT_TRUE(viewer.serve(makeDetail(double(frame)), &payload, &error)) << error;
        EXPECT_FALSE(stage.sync(payload)) << "frame " << frame;
        bool edited = false;
        ASSERT_TRUE(GeoViewerLayer::refreshLayers(payload, &edited, &error)) << error;
        EXPECT_TRUE(edited) << "frame " << frame;
        EXPECT_EQ(meshPoints(usd, "/geo/a")[0], GfVec3f(0.f, 0.f, float(frame))) << "frame " << frame;
        EXPECT_EQ(get_pointer(SdfLayer::Find(identifier)), get_pointer(layer));
    }
    EXPECT_TRUE(log.resynced.empty());
}

TEST(GeoViewerLayer, PlayedFramesWaitForTheTimeline)
{
    GeoViewerLayer viewer("viewer-test-held");
    const std::string identifier = viewer.getIdentifier();
    const GeoDetailConstPtr first = makeDetail(1.), second = makeDetail(2.), third = makeDetail(3.);
    ScenePayload payload;
    std::string error;
    ASSERT_TRUE(viewer.serveFrame(first, 1, false, &payload, &error)) << error;
    // The layer opens on the first detail served, before any frame is shown.
    EXPECT_EQ(viewer.getDetail(), first);
    int frame = 0;
    EXPECT_FALSE(GeoViewerLayer::getServedFrame(identifier, &frame));
    EXPECT_TRUE(viewer.showFrame(1));
    EXPECT_FALSE(viewer.showFrame(1));

    SceneStage stage;
    stage.sync(payload);
    const UsdStageRefPtr& usd = stage.getStage();
    NoticeLog log(usd);

    // Playback evaluates frames ahead: they change nothing until the timeline reaches them.
    ASSERT_TRUE(viewer.serveFrame(second, 2, true, &payload, &error)) << error;
    ASSERT_TRUE(viewer.serveFrame(third, 3, true, &payload, &error)) << error;
    EXPECT_EQ(viewer.getDetail(), first);
    EXPECT_FALSE(viewer.showFrame(4));
    EXPECT_TRUE(viewer.showFrame(2));
    EXPECT_EQ(viewer.getDetail(), second);
    bool edited = false;
    ASSERT_TRUE(GeoViewerLayer::refreshLayers(payload, &edited, &error)) << error;
    EXPECT_TRUE(edited);
    EXPECT_EQ(meshPoints(usd, "/geo/a")[0], GfVec3f(0.f, 0.f, 2.f));
    ASSERT_TRUE(GeoViewerLayer::getServedFrame(identifier, &frame));
    EXPECT_EQ(frame, 2);
    EXPECT_TRUE(log.resynced.empty());

    // An evaluation outside playback drops the frames held before it.
    ASSERT_TRUE(viewer.serveFrame(first, 5, false, &payload, &error)) << error;
    EXPECT_FALSE(viewer.showFrame(3));
    EXPECT_TRUE(viewer.showFrame(5));
    EXPECT_EQ(viewer.getDetail(), first);
}

TEST(GeoViewerLayer, PicksMapPrimsToPrimitiveNumbers)
{
    GeoViewerLayer viewer("viewer-test-pick");
    ScenePayload payload;
    std::string error;
    ASSERT_TRUE(viewer.serve(makeDetail(0.), &payload, &error)) << error;
    const std::shared_ptr<const GeoUsdConvert> convert = GeoViewerLayer::layerConvert(viewer.getIdentifier());
    ASSERT_TRUE(convert != nullptr);

    GeoViewerLayer::Pick pick;
    ASSERT_TRUE(GeoViewerLayer::pickPrimitives(*convert, "/geo/b", &pick));
    EXPECT_EQ(pick.primPath, "/geo/b");
    EXPECT_EQ(pick.primitives, std::vector<GeoIndex>({ 1 }));
    ASSERT_TRUE(GeoViewerLayer::pickPrimitives(*convert, "/geo/a.points", &pick));
    EXPECT_EQ(pick.primPath, "/geo/a");
    EXPECT_EQ(pick.primitives, std::vector<GeoIndex>({ 0 }));
    EXPECT_FALSE(GeoViewerLayer::pickPrimitives(*convert, "/elsewhere", &pick));
}
