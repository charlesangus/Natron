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
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
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
} // anon namespace

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
            std::lock_guard<std::mutex> lock(publishedMutex);
            published.push_back(self->getList()->generation);
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
    std::lock_guard<std::mutex> lock(publishedMutex);
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
            std::lock_guard<std::mutex> lock(publishedMutex);
            published.push_back(self->getList());
        });
    self = &boxes;

    boxes.setRules(rules({ { "/World", SceneDrawFilter::eRuleExpand } }));
    const SceneDrawBoxes::ListPtr list = latest(boxes);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->boxes.size(), 4u);
    // Four rows' transforms, then four rows' bounds.
    EXPECT_EQ(reads.load(), 8);

    std::lock_guard<std::mutex> lock(publishedMutex);
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
