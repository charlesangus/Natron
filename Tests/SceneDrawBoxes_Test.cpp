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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Gui/SceneDrawBoxes.h"

#ifdef emit
#undef emit
#endif
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/xformable.h>

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {
const std::chrono::milliseconds kWait(10000);

// /World/A and /World/Moving hold a 2-unit cube, /World/B a 4-unit one; /World/Empty has nothing to
// bound. /World/Moving goes from x=0 at time 1 to x=5 at time 2.
const char* const kScene = R"(#usda 1.0
def Xform "World"
{
    def Xform "A"
    {
        double3 xformOp:translate = (10, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            float3[] extent = [(-1, -1, -1), (1, 1, 1)]
        }
    }
    def Xform "B"
    {
        def Cube "Cube"
        {
            double size = 4
            float3[] extent = [(-2, -2, -2), (2, 2, 2)]
        }
    }
    def Xform "Moving"
    {
        double3 xformOp:translate.timeSamples = {
            1: (0, 0, 0),
            2: (5, 0, 0),
        }
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "Cube"
        {
            float3[] extent = [(-1, -1, -1), (1, 1, 1)]
        }
    }
    def Xform "Empty"
    {
        double3 xformOp:translate = (3, 4, 5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
)";

UsdStageRefPtr
openScene()
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");

    EXPECT_TRUE(layer->ImportFromString(kScene));

    return UsdStage::Open(layer);
}

std::unique_ptr<SceneDrawBoxes>
boxesOver(const UsdStageRefPtr& stage,
          const std::atomic<U64>* version = nullptr)
{
    return std::unique_ptr<SceneDrawBoxes>(new SceneDrawBoxes(
        [stage, version](const SceneDrawBoxes::StageRead& read) { read(stage, version ? version->load() : 0); },
        std::function<bool()>(),
        std::function<void()>()));
}

SceneDrawBoxes::ListPtr
latest(const SceneDrawBoxes& boxes)
{
    EXPECT_TRUE(boxes.waitForList(boxes.getGeneration(), kWait));

    return boxes.getList();
}

const SceneDrawBoxes::Box*
boxOf(const SceneDrawBoxes::ListPtr& list,
      const std::string& path)
{
    if (!list) {
        return nullptr;
    }
    for (const SceneDrawBoxes::Box& box : list->boxes) {
        if (box.path == SdfPath(path)) {
            return &box;
        }
    }

    return nullptr;
}

void
expectBox(const SceneDrawBoxes::ListPtr& list,
          const std::string& path,
          const GfVec3f& min,
          const GfVec3f& max,
          SceneDrawBoxes::KindEnum kind = SceneDrawBoxes::eKindBox)
{
    const SceneDrawBoxes::Box* box = boxOf(list, path);

    ASSERT_NE(box, nullptr) << path;
    EXPECT_EQ(box->min, min) << path;
    EXPECT_EQ(box->max, max) << path;
    EXPECT_EQ(box->kind, kind) << path;
}

SceneDrawFilter::Rules
rules(std::initializer_list<std::pair<const char*, SceneDrawFilter::RuleEnum>> entries)
{
    SceneDrawFilter::Rules result;

    for (const std::pair<const char*, SceneDrawFilter::RuleEnum>& entry : entries) {
        result[SdfPath(entry.first)] = entry.second;
    }

    return result;
}
// A 100 x 100 view from z = 10 down -Z, 60 degrees across.
GfMatrix4d
viewProjection()
{
    GfFrustum frustum;

    frustum.SetPosition(GfVec3d(0, 0, 10));
    frustum.SetPerspective(60., 1., 1., 100.);

    return frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix();
}

SceneDrawBoxes::Box
makeBox(const char* path,
        const GfVec3f& min,
        const GfVec3f& max,
        SceneDrawBoxes::KindEnum kind = SceneDrawBoxes::eKindBox)
{
    SceneDrawBoxes::Box box;

    box.path = SdfPath(path);
    box.min = min;
    box.max = max;
    box.kind = kind;

    return box;
}

int
pickAt(const SceneDrawBoxes::List& list,
       double x,
       double y,
       GfVec3d* point = nullptr)
{
    GfVec3d hit;
    const int index = SceneDrawBoxes::pick(list, viewProjection(), 100, 100, GfVec2d(x, y), 6., &hit);

    if (point) {
        *point = hit;
    }

    return index;
}
} // anon namespace

TEST(SceneDrawBoxes, APickTakesTheNearestBoxOnTheRayAtItsNearFace)
{
    SceneDrawBoxes::List list;
    list.boxes.push_back(makeBox("/Far", GfVec3f(-5, -5, -4), GfVec3f(5, 5, -2)));
    list.boxes.push_back(makeBox("/Near", GfVec3f(-1, -1, 2), GfVec3f(1, 1, 4)));

    GfVec3d point;
    EXPECT_EQ(pickAt(list, 50, 50, &point), 1);
    EXPECT_NEAR(point[2], 4., 1e-6);
    // Past /Near's side, still inside /Far.
    EXPECT_EQ(pickAt(list, 67.5, 50, &point), 0);
    EXPECT_NEAR(point[2], -2., 1e-6);
    EXPECT_EQ(pickAt(list, 99, 1), -1);
}

TEST(SceneDrawBoxes, ABoxAroundTheEyeIsNeverPicked)
{
    SceneDrawBoxes::List list;
    list.boxes.push_back(makeBox("/Around", GfVec3f(-50, -50, -50), GfVec3f(50, 50, 50)));

    EXPECT_EQ(pickAt(list, 50, 50), -1);
    list.boxes.push_back(makeBox("/Near", GfVec3f(-1, -1, 2), GfVec3f(1, 1, 4)));
    EXPECT_EQ(pickAt(list, 50, 50), 1);
}

TEST(SceneDrawBoxes, AMarkerIsPickedNearItsCentreOnScreenAndByDepthAgainstBoxes)
{
    SceneDrawBoxes::List list;
    list.boxes.push_back(makeBox("/Marker", GfVec3f(0, 0, 0), GfVec3f(0, 0, 0), SceneDrawBoxes::eKindMarker));

    GfVec3d point;
    EXPECT_EQ(pickAt(list, 53, 47, &point), 0);
    EXPECT_EQ(point, GfVec3d(0, 0, 0));
    EXPECT_EQ(pickAt(list, 60, 50), -1);

    list.boxes.push_back(makeBox("/Near", GfVec3f(-1, -1, 2), GfVec3f(1, 1, 4)));
    EXPECT_EQ(pickAt(list, 50, 50), 1);
    list.boxes.push_back(makeBox("/Pending", GfVec3f(0, 0, 6), GfVec3f(0, 0, 6), SceneDrawBoxes::eKindPending));
    EXPECT_EQ(pickAt(list, 50, 50), 2);
}

TEST(SceneDrawBoxes, ABBoxRowAndAChildrenRowsUnruledChildrenGetTheirWorldBounds)
{
    const UsdStageRefPtr stage = openScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setTime(1.);
    boxes->setRules(rules({ { "/World/A", SceneDrawFilter::eRuleHide } }));
    SceneDrawBoxes::ListPtr list = latest(*boxes);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->boxes.size(), 1u);
    expectBox(list, "/World/A", GfVec3f(9, -1, -1), GfVec3f(11, 1, 1));

    boxes->setRules(rules({ { "/World", SceneDrawFilter::eRuleExpand }, { "/World/B", SceneDrawFilter::eRuleShow } }));
    list = latest(*boxes);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->boxes.size(), 3u);
    expectBox(list, "/World/A", GfVec3f(9, -1, -1), GfVec3f(11, 1, 1));
    expectBox(list, "/World/Moving", GfVec3f(-1, -1, -1), GfVec3f(1, 1, 1));
    expectBox(list, "/World/Empty", GfVec3f(3, 4, 5), GfVec3f(3, 4, 5), SceneDrawBoxes::eKindMarker);
    EXPECT_EQ(boxOf(list, "/World/B"), nullptr);
}

TEST(SceneDrawBoxes, HiddenRowsAndRowsUnderAHidingRowHaveNoBox)
{
    const UsdStageRefPtr stage = openScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setRules(rules({ { "/World/A", SceneDrawFilter::eRuleHidden }, { "/World/Nowhere", SceneDrawFilter::eRuleHide } }));
    SceneDrawBoxes::ListPtr list = latest(*boxes);
    ASSERT_NE(list, nullptr);
    EXPECT_TRUE(list->boxes.empty());

    boxes->setRules(rules({ { "/World", SceneDrawFilter::eRuleHide }, { "/World/A", SceneDrawFilter::eRuleHide } }));
    list = latest(*boxes);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->boxes.size(), 1u);
    expectBox(list, "/World", GfVec3f(-2, -2, -2), GfVec3f(11, 2, 2));
}

TEST(SceneDrawBoxes, AFrameChangeRecomputesOnlyAnimatedRows)
{
    const UsdStageRefPtr stage = openScene();
    std::atomic<U64> version { 0 };
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage, &version);

    boxes->setTime(1.);
    boxes->setRules(rules({ { "/World/A", SceneDrawFilter::eRuleHide }, { "/World/Moving", SceneDrawFilter::eRuleHide } }));
    SceneDrawBoxes::ListPtr list = latest(*boxes);
    expectBox(list, "/World/Moving", GfVec3f(-1, -1, -1), GfVec3f(1, 1, 1));

    // Unannounced, so only a row read again can show it.
    bool resetsStack = false;
    UsdGeomXformable(stage->GetPrimAtPath(SdfPath("/World/A"))).GetOrderedXformOps(&resetsStack)[0].Set(GfVec3d(20, 0, 0));
    boxes->setTime(2.);
    list = latest(*boxes);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->time, 2.);
    expectBox(list, "/World/Moving", GfVec3f(4, -1, -1), GfVec3f(6, 1, 1));
    expectBox(list, "/World/A", GfVec3f(9, -1, -1), GfVec3f(11, 1, 1));

    version = 1;
    boxes->setStageVersion(1);
    list = latest(*boxes);
    expectBox(list, "/World/A", GfVec3f(19, -1, -1), GfVec3f(21, 1, 1));
}

TEST(SceneDrawBoxes, AListFromRulesThatChangedDuringItsPassIsDropped)
{
    const UsdStageRefPtr stage = openScene();
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    std::mutex publishedMutex;
    std::condition_variable publishedChanged;
    std::vector<U64> published;
    SceneDrawBoxes* self = nullptr;
    SceneDrawBoxes boxes(
        [&](const SceneDrawBoxes::StageRead& read) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&]() { return released; });
            }
            read(stage, 0);
        },
        std::function<bool()>(),
        [&]() {
            {
                std::lock_guard<std::mutex> lock(publishedMutex);
                published.push_back(self->getList()->generation);
            }
            publishedChanged.notify_all();
        });
    self = &boxes;

    boxes.setRules(rules({ { "/World/A", SceneDrawFilter::eRuleHide } }));
    const U64 stale = boxes.getGeneration();
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, kWait, [&]() { return entered; }));
    }
    boxes.setRules(rules({ { "/World/B", SceneDrawFilter::eRuleHide } }));
    const U64 fresh = boxes.getGeneration();
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
    }
    changed.notify_all();
    ASSERT_TRUE(boxes.waitForList(fresh, kWait));
    const SceneDrawBoxes::ListPtr list = boxes.getList();
    ASSERT_EQ(list->boxes.size(), 1u);
    expectBox(list, "/World/B", GfVec3f(-2, -2, -2), GfVec3f(2, 2, 2));
    // The list is out before the callback runs, so the wait above can end ahead of it.
    std::unique_lock<std::mutex> lock(publishedMutex);
    ASSERT_TRUE(publishedChanged.wait_for(lock, kWait, [&]() { return !published.empty(); }));
    EXPECT_EQ(published, std::vector<U64>({ fresh }));
    EXPECT_NE(stale, fresh);
}

TEST(SceneDrawBoxes, AWaitingWriterEndsTheHoldAfterEachRowAndTheRowsToComeShowAsPending)
{
    const UsdStageRefPtr stage = openScene();
    std::atomic<int> reads { 0 };
    // A writer arrives during every hold and is gone once the hold lets go.
    std::atomic<bool> writerWaiting { false };
    std::mutex publishedMutex;
    std::condition_variable publishedChanged;
    std::vector<SceneDrawBoxes::ListPtr> published;
    SceneDrawBoxes* self = nullptr;
    SceneDrawBoxes boxes(
        [&](const SceneDrawBoxes::StageRead& read) {
            ++reads;
            writerWaiting = true;
            read(stage, 0);
        },
        [&]() { return writerWaiting.exchange(false); },
        [&]() {
            {
                std::lock_guard<std::mutex> lock(publishedMutex);
                published.push_back(self->getList());
            }
            publishedChanged.notify_all();
        });
    self = &boxes;

    boxes.setRules(rules({ { "/World", SceneDrawFilter::eRuleExpand } }));
    const SceneDrawBoxes::ListPtr list = latest(boxes);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->boxes.size(), 4u);
    // Four rows' transforms, then four rows' bounds.
    EXPECT_EQ(reads.load(), 8);

    std::unique_lock<std::mutex> lock(publishedMutex);
    ASSERT_TRUE(publishedChanged.wait_for(lock, kWait, [&]() { return published.size() >= 2u; }));
    ASSERT_EQ(published.size(), 2u);
    const SceneDrawBoxes::ListPtr& early = published.front();
    EXPECT_FALSE(early->complete);
    ASSERT_EQ(early->boxes.size(), 4u);
    expectBox(early, "/World/A", GfVec3f(9, -1, -1), GfVec3f(11, 1, 1));
    expectBox(early, "/World/B", GfVec3f(0, 0, 0), GfVec3f(0, 0, 0), SceneDrawBoxes::eKindPending);
    expectBox(early, "/World/Empty", GfVec3f(3, 4, 5), GfVec3f(3, 4, 5), SceneDrawBoxes::eKindPending);
    EXPECT_TRUE(published.back()->complete);
    expectBox(published.back(), "/World/B", GfVec3f(-2, -2, -2), GfVec3f(2, 2, 2));
}

TEST(SceneDrawBoxes, AnUnloadedPayloadIsBoxedByItsHintAndMarkedWithoutOne)
{
    SdfLayerRefPtr asset = SdfLayer::CreateAnonymous(".usda");
    ASSERT_TRUE(asset->ImportFromString(R"(#usda 1.0
(
    defaultPrim = "Asset"
)
def Xform "Asset"
{
    def Cube "Cube"
    {
        float3[] extent = [(-1, -1, -1), (1, 1, 1)]
    }
}
)"));
    SdfLayerRefPtr root = SdfLayer::CreateAnonymous(".usda");
    const std::string payload = "@" + asset->GetIdentifier() + "@";
    ASSERT_TRUE(root->ImportFromString(std::string("#usda 1.0\n") + "def Xform \"City\" (\n    kind = \"group\"\n)\n{\n"
                                                                    "    def Xform \"Hinted\" (\n        kind = \"component\"\n        payload = "
                                       + payload + "\n    )\n    {\n"
                                                   "        float3[] extentsHint = [(0, 0, 0), (2, 2, 2)]\n    }\n"
                                                   "    def Xform \"Plain\" (\n        payload = "
                                       + payload + "\n    )\n    {\n"
                                                   "        double3 xformOp:translate = (5, 0, 0)\n"
                                                   "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n    }\n"
                                                   "}\n"));
    const UsdStageRefPtr stage = UsdStage::Open(root, UsdStage::LoadNone);
    ASSERT_TRUE(stage);
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setRules(rules({ { "/City", SceneDrawFilter::eRuleExpand } }));
    SceneDrawBoxes::ListPtr list = latest(*boxes);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->boxes.size(), 2u);
    expectBox(list, "/City/Hinted", GfVec3f(0, 0, 0), GfVec3f(2, 2, 2), SceneDrawBoxes::eKindUnloaded);
    expectBox(list, "/City/Plain", GfVec3f(5, 0, 0), GfVec3f(5, 0, 0), SceneDrawBoxes::eKindMarker);
    EXPECT_FALSE(stage->GetPrimAtPath(SdfPath("/City/Hinted")).IsLoaded());
}

namespace {
// /World/Cube is a six-face mesh, /World/Lines two linear curves of 3 and 4 vertices, /World/Bezier
// one cubic Bezier curve of 7, /World/Cloud three points, and /World/Instancer three instances of a
// six-face prototype along X.
const char* const kCountScene = R"(#usda 1.0
def Xform "World"
{
    def Mesh "Cube"
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
        point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]
        float3[] extent = [(-1, -1, -1), (1, 1, 1)]
    }
    def BasisCurves "Lines"
    {
        uniform token type = "linear"
        int[] curveVertexCounts = [3, 4]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0), (3, 1, 0)]
        float3[] extent = [(0, 0, 0), (3, 1, 0)]
    }
    def BasisCurves "Bezier"
    {
        uniform token type = "cubic"
        uniform token basis = "bezier"
        int[] curveVertexCounts = [7]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (3, 0, 0), (4, 0, 0), (5, 0, 0), (6, 0, 0)]
        float3[] extent = [(0, 0, 0), (6, 0, 0)]
    }
    def Points "Cloud"
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0)]
        float3[] extent = [(0, 0, 0), (2, 0, 0)]
    }
    def PointInstancer "Instancer"
    {
        rel prototypes = [</World/Instancer/Prototypes/Box>]
        int[] protoIndices = [0, 0, 0]
        point3f[] positions = [(0, 0, 0), (10, 0, 0), (20, 0, 0)]
        def Scope "Prototypes"
        {
            def Mesh "Box"
            {
                int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
                int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
                point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]
                float3[] extent = [(-1, -1, -1), (1, 1, 1)]
            }
        }
    }
}
)";

UsdStageRefPtr
openCountScene()
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");

    EXPECT_TRUE(layer->ImportFromString(kCountScene));

    return UsdStage::Open(layer);
}

SceneDrawBoxes::UnitsPtr
latestUnits(const SceneDrawBoxes& boxes)
{
    EXPECT_TRUE(boxes.waitForUnits(boxes.getGeneration(), kWait));

    return boxes.getUnits();
}

const SceneDrawBoxes::Unit*
unitOf(const SceneDrawBoxes::UnitsPtr& units,
       const std::string& path)
{
    if (!units) {
        return nullptr;
    }
    for (const SceneDrawBoxes::Unit& unit : units->units) {
        if (unit.path == SdfPath(path)) {
            return &unit;
        }
    }

    return nullptr;
}
} // anon namespace

TEST(SceneDrawBoxes, NoUnitsAreCountedUntilCountingIsOn)
{
    const UsdStageRefPtr stage = openCountScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setTime(1.);
    ASSERT_NE(latest(*boxes), nullptr);
    EXPECT_EQ(boxes->getUnits(), nullptr);

    boxes->setCounting(true);
    EXPECT_NE(latestUnits(*boxes), nullptr);
    boxes->setCounting(false);
    EXPECT_EQ(boxes->getUnits(), nullptr);
}

TEST(SceneDrawBoxes, MeshesCountFacesCurvesSegmentsAndPointsPoints)
{
    const UsdStageRefPtr stage = openCountScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setCounting(true);
    const SceneDrawBoxes::UnitsPtr units = latestUnits(*boxes);
    ASSERT_NE(units, nullptr);

    const SceneDrawBoxes::Unit* cube = unitOf(units, "/World/Cube");
    ASSERT_NE(cube, nullptr);
    EXPECT_EQ(cube->faces, 6u);
    EXPECT_TRUE(cube->bounded);
    EXPECT_EQ(cube->state, SceneViewBudget::eStateDrawn);
    EXPECT_TRUE(cube->mesh);
    EXPECT_EQ(cube->min, GfVec3f(-1, -1, -1));
    EXPECT_EQ(cube->max, GfVec3f(1, 1, 1));
    EXPECT_EQ(cube->instances, nullptr);

    const SceneDrawBoxes::Unit* lines = unitOf(units, "/World/Lines");
    ASSERT_NE(lines, nullptr);
    EXPECT_EQ(lines->faces, 5u);
    EXPECT_FALSE(lines->mesh);
    const SceneDrawBoxes::Unit* bezier = unitOf(units, "/World/Bezier");
    ASSERT_NE(bezier, nullptr);
    EXPECT_EQ(bezier->faces, 2u);
    const SceneDrawBoxes::Unit* cloud = unitOf(units, "/World/Cloud");
    ASSERT_NE(cloud, nullptr);
    EXPECT_EQ(cloud->faces, 3u);
    EXPECT_EQ(unitOf(units, "/World"), nullptr);
}

TEST(SceneDrawBoxes, AnInstancerIsOneUnitWithItsPrototypesAndInstanceArrays)
{
    const UsdStageRefPtr stage = openCountScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setCounting(true);
    const SceneDrawBoxes::UnitsPtr units = latestUnits(*boxes);
    const SceneDrawBoxes::Unit* instancer = unitOf(units, "/World/Instancer");
    ASSERT_NE(instancer, nullptr);
    EXPECT_EQ(instancer->faces, 18u);
    EXPECT_EQ(instancer->min, GfVec3f(-1, -1, -1));
    EXPECT_EQ(instancer->max, GfVec3f(21, 1, 1));
    ASSERT_NE(instancer->instances, nullptr);
    const SceneDrawBoxes::Instances& instances = *instancer->instances;
    ASSERT_EQ(instances.size(), 3u);
    ASSERT_EQ(instances.prototypes.size(), 1u);
    EXPECT_EQ(instances.prototypes[0].path, SdfPath("/World/Instancer/Prototypes/Box"));
    EXPECT_EQ(instances.prototypes[0].faces, 6u);
    EXPECT_TRUE(instances.prototypes[0].bounded);
    EXPECT_EQ(instances.prototypes[0].min, GfVec3f(-1, -1, -1));
    EXPECT_EQ(instances.prototypes[0].max, GfVec3f(1, 1, 1));
    EXPECT_TRUE(instances.draws(2));
    EXPECT_EQ(instances.instanceTransform(1).ExtractTranslation(), GfVec3d(10, 0, 0));
    // The prototype draws through its instancer alone.
    EXPECT_EQ(unitOf(units, "/World/Instancer/Prototypes/Box"), nullptr);
}

TEST(SceneDrawBoxes, AnInstanceTheInstancerTurnsOffCountsNoFaces)
{
    const UsdStageRefPtr stage = openCountScene();
    UsdGeomPointInstancer(stage->GetPrimAtPath(SdfPath("/World/Instancer"))).CreateInvisibleIdsAttr().Set(VtInt64Array { 2 });
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setCounting(true);
    const SceneDrawBoxes::Unit* instancer = unitOf(latestUnits(*boxes), "/World/Instancer");
    ASSERT_NE(instancer, nullptr);
    EXPECT_EQ(instancer->faces, 12u);
    ASSERT_NE(instancer->instances, nullptr);
    EXPECT_TRUE(instancer->instances->draws(1));
    EXPECT_FALSE(instancer->instances->draws(2));
}

TEST(SceneDrawBoxes, OnlyWhatTheRulesDrawIsAUnit)
{
    const UsdStageRefPtr stage = openCountScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setCounting(true);
    boxes->setRules(rules({ { "/World/Cube", SceneDrawFilter::eRuleHide }, { "/World/Lines", SceneDrawFilter::eRuleHidden } }));
    SceneDrawBoxes::UnitsPtr units = latestUnits(*boxes);
    ASSERT_NE(units, nullptr);
    EXPECT_EQ(unitOf(units, "/World/Cube"), nullptr);
    EXPECT_EQ(unitOf(units, "/World/Lines"), nullptr);
    EXPECT_NE(unitOf(units, "/World/Bezier"), nullptr);

    boxes->setRules(rules({ { "/World", SceneDrawFilter::eRuleExpand }, { "/World/Cube", SceneDrawFilter::eRuleShow } }));
    units = latestUnits(*boxes);
    ASSERT_NE(units, nullptr);
    ASSERT_EQ(units->units.size(), 1u);
    EXPECT_EQ(units->units[0].path, SdfPath("/World/Cube"));
}

TEST(SceneDrawBoxes, AnUnloadedPayloadCountsNoFacesUntilItLoads)
{
    SdfLayerRefPtr asset = SdfLayer::CreateAnonymous(".usda");
    ASSERT_TRUE(asset->ImportFromString(R"(#usda 1.0
(
    defaultPrim = "Asset"
)
def Xform "Asset"
{
    def Cube "Cube"
    {
        float3[] extent = [(-1, -1, -1), (1, 1, 1)]
    }
}
)"));
    SdfLayerRefPtr root = SdfLayer::CreateAnonymous(".usda");
    const std::string payload = "@" + asset->GetIdentifier() + "@";
    ASSERT_TRUE(root->ImportFromString(std::string("#usda 1.0\n") + "def Xform \"City\" (\n    kind = \"group\"\n)\n{\n"
                                                                    "    def Xform \"Hinted\" (\n        kind = \"component\"\n        payload = "
                                       + payload + "\n    )\n    {\n"
                                                   "        float3[] extentsHint = [(0, 0, 0), (2, 2, 2)]\n    }\n"
                                                   "}\n"));
    const UsdStageRefPtr stage = UsdStage::Open(root, UsdStage::LoadNone);
    ASSERT_TRUE(stage);
    std::atomic<U64> version { 0 };
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage, &version);

    boxes->setCounting(true);
    SceneDrawBoxes::UnitsPtr units = latestUnits(*boxes);
    const SceneDrawBoxes::Unit* hinted = unitOf(units, "/City/Hinted");
    ASSERT_NE(hinted, nullptr);
    EXPECT_EQ(hinted->state, SceneViewBudget::eStateUnloaded);
    EXPECT_TRUE(hinted->bounded);
    EXPECT_EQ(hinted->faces, 0u);
    EXPECT_EQ(hinted->min, GfVec3f(0, 0, 0));
    EXPECT_EQ(hinted->max, GfVec3f(2, 2, 2));
    EXPECT_EQ(unitOf(units, "/City/Hinted/Cube"), nullptr);

    stage->Load(SdfPath("/City/Hinted"));
    version = 1;
    boxes->setStageVersion(1);
    units = latestUnits(*boxes);
    EXPECT_EQ(unitOf(units, "/City/Hinted"), nullptr);
    const SceneDrawBoxes::Unit* cube = unitOf(units, "/City/Hinted/Cube");
    ASSERT_NE(cube, nullptr);
    EXPECT_EQ(cube->faces, 6u);
}

TEST(SceneDrawBoxes, TheCameraIsKeptWithoutStartingAPass)
{
    const UsdStageRefPtr stage = openCountScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    const U64 generation = boxes->getGeneration();
    SceneDrawBoxes::Camera camera;
    camera.eye = GfVec3d(0, 0, 10);
    camera.viewProjection = viewProjection();
    camera.valid = true;
    boxes->setCamera(camera);
    EXPECT_TRUE(boxes->getCamera() == camera);
    EXPECT_EQ(boxes->getGeneration(), generation);
}

TEST(SceneDrawBoxes, AHugeInstancersBoundAndFacesComeFromEveryChunkOfItsInstances)
{
    const UsdStageRefPtr stage = openCountScene();
    const UsdGeomPointInstancer instancer(stage->GetPrimAtPath(SdfPath("/World/Instancer")));
    ASSERT_TRUE(bool(instancer));
    const int count = 200000;
    VtIntArray protoIndices(count, 0);
    VtVec3fArray positions(count);
    for (int index = 0; index < count; ++index) {
        positions[index] = GfVec3f(float(index), 0.f, 0.f);
    }
    instancer.GetProtoIndicesAttr().Set(protoIndices);
    instancer.GetPositionsAttr().Set(positions);
    instancer.CreateInvisibleIdsAttr().Set(VtInt64Array { 7, count - 1 });
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setCounting(true);
    const SceneDrawBoxes::UnitsPtr units = latestUnits(*boxes);
    const SceneDrawBoxes::Unit* unit = unitOf(units, "/World/Instancer");
    ASSERT_NE(unit, nullptr);
    EXPECT_EQ(unit->faces, 6u * U64(count - 2));
    EXPECT_TRUE(unit->bounded);
    EXPECT_EQ(unit->min, GfVec3f(-1, -1, -1));
    EXPECT_EQ(unit->max, GfVec3f(float(count - 2) + 1.f, 1, 1));
}

namespace {
SceneDrawBoxes::Camera
budgetCamera()
{
    SceneDrawBoxes::Camera camera;
    camera.eye = GfVec3d(0, 0, 10);
    camera.viewProjection = viewProjection();
    camera.valid = true;

    return camera;
}
} // anon namespace

TEST(SceneDrawBoxes, ARequestedBudgetIsSolvedOnTheWorkerAgainstTheUnitsAndStoppingDropsIt)
{
    const UsdStageRefPtr stage = openCountScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);
    SceneViewBudget::Settings settings;
    settings.limit = 10;
    settings.countWholeScene = true;

    const U64 serial = boxes->requestBudget(budgetCamera(), settings);

    ASSERT_TRUE(boxes->waitForBudget(serial, kWait));
    const SceneDrawBoxes::BudgetPtr budget = boxes->getBudget();
    ASSERT_NE(budget, nullptr);
    EXPECT_GE(budget->serial, serial);
    EXPECT_TRUE(budget->result.valid);
    EXPECT_EQ(budget->result.limit, 10u);
    EXPECT_GT(budget->result.counted, 10u);
    EXPECT_GT(budget->result.demotedCount, 0u);
    EXPECT_FALSE(budget->filter.empty());
    ASSERT_NE(budget->units, nullptr);
    EXPECT_NE(unitOf(budget->units, "/World/Cube"), nullptr);

    boxes->stopBudget();

    EXPECT_EQ(boxes->getBudget(), nullptr);
}

TEST(SceneDrawBoxes, ALiveBudgetIsSolvedAgainWhenTheUnitsChangeAndACancelledOneIsNot)
{
    const UsdStageRefPtr stage = openCountScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);
    SceneViewBudget::Settings settings;
    settings.countWholeScene = true;

    const U64 serial = boxes->requestBudget(budgetCamera(), settings);
    ASSERT_TRUE(boxes->waitForBudget(serial, kWait));
    const SceneDrawBoxes::BudgetPtr first = boxes->getBudget();
    EXPECT_EQ(first->result.demotedCount, 0u);

    boxes->setRules(rules({ { "/World/Cube", SceneDrawFilter::eRuleHide } }));
    ASSERT_TRUE(boxes->waitForBudget(first->serial + 1, kWait));
    const SceneDrawBoxes::BudgetPtr again = boxes->getBudget();
    EXPECT_EQ(unitOf(again->units, "/World/Cube"), nullptr);

    boxes->cancelBudget();
    boxes->setRules(SceneDrawFilter::Rules());
    latestUnits(*boxes);

    EXPECT_FALSE(boxes->waitForBudget(again->serial + 1, std::chrono::milliseconds(200)));
    EXPECT_EQ(boxes->getBudget(), again);
}

TEST(SceneDrawBoxes, TheOrderPutsWhatCoversMoreOfTheViewFirstAndTheNearerOfTwoThatCoverNothing)
{
    SceneDrawBoxes::List list;
    list.boxes.push_back(makeBox("/Small", GfVec3f(-0.5f, -0.5f, -10), GfVec3f(0.5f, 0.5f, -9)));
    list.boxes.push_back(makeBox("/Behind", GfVec3f(-1, -1, 20), GfVec3f(1, 1, 22)));
    list.boxes.push_back(makeBox("/Big", GfVec3f(-3, -3, -2), GfVec3f(3, 3, 0)));
    list.boxes.push_back(makeBox("/FarAside", GfVec3f(200, 0, -60), GfVec3f(201, 1, -59)));
    list.boxes.push_back(makeBox("/NearAside", GfVec3f(50, 0, 0), GfVec3f(51, 1, 1)));
    list.boxes.push_back(makeBox("/Around", GfVec3f(-50, -50, -50), GfVec3f(50, 50, 50)));
    list.boxes.push_back(makeBox("/Marker", GfVec3f(0, 0, 5), GfVec3f(0, 0, 5), SceneDrawBoxes::eKindMarker));

    EXPECT_DOUBLE_EQ(SceneDrawBoxes::screenArea(list.boxes[5], viewProjection(), 100, 100), 10000.);
    EXPECT_GT(SceneDrawBoxes::screenArea(list.boxes[2], viewProjection(), 100, 100), SceneDrawBoxes::screenArea(list.boxes[0], viewProjection(), 100, 100));
    EXPECT_EQ(SceneDrawBoxes::screenArea(list.boxes[1], viewProjection(), 100, 100), 0.);
    EXPECT_EQ(SceneDrawBoxes::screenArea(list.boxes[4], viewProjection(), 100, 100), 0.);

    const std::vector<SdfPath> order = SceneDrawBoxes::nearFirst(list, viewProjection(), 100, 100);
    const std::vector<SdfPath> expected = { SdfPath("/Around"), SdfPath("/Big"), SdfPath("/Small"), SdfPath("/Marker"), SdfPath("/NearAside"), SdfPath("/FarAside"), SdfPath("/Behind") };
    EXPECT_EQ(order, expected);
}

TEST(SceneDrawBoxes, TheBoxesOfAFillLeaveOutWhatIsAdmitted)
{
    SceneDrawBoxes::List list;
    list.generation = 3;
    list.boxes.push_back(makeBox("/A", GfVec3f(-1, -1, -1), GfVec3f(1, 1, 1)));
    list.boxes.push_back(makeBox("/B", GfVec3f(2, 2, 2), GfVec3f(3, 3, 3)));
    list.boxes.push_back(makeBox("/C", GfVec3f(-3, -1, -1), GfVec3f(-2, 1, 1)));

    const SceneDrawBoxes::List left = SceneDrawBoxes::unadmitted(list, [](const SdfPath& path) { return path != SdfPath("/A"); });

    EXPECT_EQ(left.generation, 3u);
    ASSERT_EQ(left.boxes.size(), 2u);
    EXPECT_EQ(left.boxes[0].path, SdfPath("/B"));
    EXPECT_EQ(left.boxes[1].path, SdfPath("/C"));
    // A prim not yet admitted is picked through its box.
    EXPECT_EQ(pickAt(list, 50, 50), 0);
    EXPECT_EQ(pickAt(left, 50, 50), -1);
}

TEST(SceneDrawBoxes, TheWorkerOrdersItsListForTheViewItIsGivenAndAgainForANewList)
{
    const UsdStageRefPtr stage = openScene();
    const std::unique_ptr<SceneDrawBoxes> boxes = boxesOver(stage);

    boxes->setRules(rules({ { "/World/A", SceneDrawFilter::eRuleHide }, { "/World/B", SceneDrawFilter::eRuleHide } }));
    ASSERT_NE(latest(*boxes), nullptr);
    EXPECT_EQ(boxes->getOrder(), nullptr);

    // From x = 10 looking down -Z, /World/A is right ahead and /World/B is off to the left.
    GfFrustum frustum;
    frustum.SetPosition(GfVec3d(10, 0, 10));
    frustum.SetPerspective(60., 1., 1., 100.);
    boxes->setOrderView(frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix(), 100, 100);
    ASSERT_TRUE(boxes->waitForOrder(kWait));
    SceneDrawFilter::Order order = boxes->getOrder();
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(*order, (std::vector<SdfPath> { SdfPath("/World/A"), SdfPath("/World/B") }));

    boxes->setOrderView(viewProjection(), 100, 100);
    ASSERT_TRUE(boxes->waitForOrder(kWait));
    order = boxes->getOrder();
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(*order, (std::vector<SdfPath> { SdfPath("/World/B"), SdfPath("/World/A") }));

    boxes->setRules(rules({ { "/World/A", SceneDrawFilter::eRuleHide } }));
    ASSERT_NE(latest(*boxes), nullptr);
    ASSERT_TRUE(boxes->waitForOrder(kWait));
    order = boxes->getOrder();
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(*order, (std::vector<SdfPath> { SdfPath("/World/A") }));
}
