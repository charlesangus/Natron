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

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneKindWalk.h"

NATRON_NAMESPACE_USING

namespace {
// natronTestTree derives from component and natronTestSet from group; Tests/fixtures/sceneKinds
// registers both before any test runs.
const PXR_NS::TfToken kTree("natronTestTree");
const PXR_NS::TfToken kSet("natronTestSet");

PXR_NS::UsdStageRefPtr
openStage(const std::string& body,
          PXR_NS::UsdStage::InitialLoadSet load = PXR_NS::UsdStage::LoadAll)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");

    EXPECT_TRUE(layer->ImportFromString("#usda 1.0\n" + body));

    return PXR_NS::UsdStage::Open(layer, load);
}

// A world of two assemblies: a city holding a nested block assembly with two component houses,
// one of which holds a subcomponent door, and a park whose trees are natronTestTree, beside an
// unkinded prop scope holding a component lamp.
const char* const kWorld = R"(
def Xform "World" (kind = "group")
{
    def Xform "City" (kind = "assembly")
    {
        def Xform "Block" (kind = "assembly")
        {
            def Xform "HouseA" (kind = "component")
            {
                def Xform "Door" (kind = "subcomponent")
                {
                }
            }
            def Xform "HouseB" (kind = "component")
            {
            }
        }
    }
    def Xform "Park" (kind = "assembly")
    {
        def Xform "Grove" (kind = "natronTestSet")
        {
            def Xform "Oak" (kind = "natronTestTree")
            {
                def Xform "Branch" (kind = "subcomponent")
                {
                }
            }
        }
        def Xform "Bench" (kind = "component")
        {
        }
    }
    def Scope "Props"
    {
        def Xform "Lamp" (kind = "component")
        {
        }
    }
}
)";

std::vector<std::string>
strings(const PXR_NS::SdfPathVector& paths)
{
    std::vector<std::string> out;

    for (const PXR_NS::SdfPath& path : paths) {
        out.push_back(path.GetString());
    }

    return out;
}

PXR_NS::SdfPathVector
paths(const std::vector<std::string>& strings)
{
    PXR_NS::SdfPathVector out;

    for (const std::string& s : strings) {
        out.push_back(PXR_NS::SdfPath(s));
    }

    return out;
}
} // anonymous namespace

TEST(SceneKindWalk, LevelsFollowTheKindHierarchy)
{
    EXPECT_EQ(eSceneKindLevelAssembly, sceneKindLevel(PXR_NS::KindTokens->assembly));
    EXPECT_EQ(eSceneKindLevelComponent, sceneKindLevel(PXR_NS::KindTokens->component));
    EXPECT_EQ(eSceneKindLevelSubcomponent, sceneKindLevel(PXR_NS::KindTokens->subcomponent));
    EXPECT_EQ(eSceneKindLevelPassThrough, sceneKindLevel(PXR_NS::KindTokens->group));
    EXPECT_EQ(eSceneKindLevelPassThrough, sceneKindLevel(PXR_NS::KindTokens->model));
    EXPECT_EQ(eSceneKindLevelPassThrough, sceneKindLevel(PXR_NS::TfToken()));
    EXPECT_EQ(eSceneKindLevelPassThrough, sceneKindLevel(PXR_NS::TfToken("notAKind")));
    EXPECT_EQ(eSceneKindLevelComponent, sceneKindLevel(kTree));
    EXPECT_EQ(eSceneKindLevelPassThrough, sceneKindLevel(kSet));
}

TEST(SceneKindWalk, CustomKindsAreTheRegisteredNonBuiltInOnes)
{
    const std::vector<PXR_NS::TfToken> kinds = registeredCustomKinds();

    EXPECT_NE(kinds.end(), std::find(kinds.begin(), kinds.end(), kTree));
    EXPECT_NE(kinds.end(), std::find(kinds.begin(), kinds.end(), kSet));
    for (const PXR_NS::TfToken& builtIn : { PXR_NS::KindTokens->model, PXR_NS::KindTokens->group, PXR_NS::KindTokens->assembly,
                                            PXR_NS::KindTokens->component, PXR_NS::KindTokens->subcomponent }) {
        EXPECT_EQ(kinds.end(), std::find(kinds.begin(), kinds.end(), builtIn)) << builtIn.GetString();
    }
    EXPECT_TRUE(std::is_sorted(kinds.begin(), kinds.end(), [](const PXR_NS::TfToken& a, const PXR_NS::TfToken& b) {
        return a.GetString() < b.GetString();
    }));
    EXPECT_TRUE(isKindScope(kTree));
    EXPECT_TRUE(isKindScope(PXR_NS::KindTokens->component));
    EXPECT_FALSE(isKindScope(PXR_NS::KindTokens->group));
    EXPECT_FALSE(isKindScope(PXR_NS::KindTokens->model));
    EXPECT_FALSE(isKindScope(PXR_NS::TfToken("notAKind")));
}

TEST(SceneKindWalk, AssemblyStopsAtTheOutermostAssemblies)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World" }), PXR_NS::KindTokens->assembly);

    EXPECT_EQ(std::vector<std::string>({ "/World/City", "/World/Park" }), strings(result.matches));
    EXPECT_FALSE(result.cancelled);
}

TEST(SceneKindWalk, ComponentGoesThroughAssembliesAndUnkindedScopes)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World" }), PXR_NS::KindTokens->component);

    // The tree is a component by derivation; the walk stops there and never reaches its branch.
    EXPECT_EQ(std::vector<std::string>({ "/World/City/Block/HouseA", "/World/City/Block/HouseB", "/World/Park/Grove/Oak", "/World/Park/Bench",
                                         "/World/Props/Lamp" }),
              strings(result.matches));
}

TEST(SceneKindWalk, SubcomponentDescendsThroughComponents)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World" }), PXR_NS::KindTokens->subcomponent);

    EXPECT_EQ(std::vector<std::string>({ "/World/City/Block/HouseA/Door", "/World/Park/Grove/Oak/Branch" }), strings(result.matches));
}

TEST(SceneKindWalk, AssemblyStopsAtAComponentReachedFirst)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World/Props" }), PXR_NS::KindTokens->assembly);

    EXPECT_EQ(std::vector<std::string>(), strings(result.matches));
}

TEST(SceneKindWalk, SubcomponentFromInsideAComponent)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World/City/Block/HouseA" }), PXR_NS::KindTokens->subcomponent);

    EXPECT_EQ(std::vector<std::string>({ "/World/City/Block/HouseA/Door" }), strings(result.matches));
}

TEST(SceneKindWalk, ARootOfTheTargetKindWithNoneBelowMatchesNothing)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World/City/Block/HouseA/Door" }), PXR_NS::KindTokens->subcomponent);

    EXPECT_EQ(std::vector<std::string>(), strings(result.matches));
}

TEST(SceneKindWalk, CustomKindMatchesExactlyAtItsBaseLevel)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World" }), kTree);

    // The houses, bench and lamp are components: the walk stops at them without a match.
    EXPECT_EQ(std::vector<std::string>({ "/World/Park/Grove/Oak" }), strings(result.matches));
}

TEST(SceneKindWalk, CustomKindWithNoLevelStopsOnlyWhereItMatches)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World" }), kSet);

    EXPECT_EQ(std::vector<std::string>({ "/World/Park/Grove" }), strings(result.matches));
}

TEST(SceneKindWalk, CustomKindWithNoLevelWalksUnmatchedBranchesToTheBottom)
{
    PXR_NS::SdfLayerRefPtr door = PXR_NS::SdfLayer::CreateAnonymous(".usda");

    ASSERT_TRUE(door->ImportFromString("#usda 1.0\n(\n    defaultPrim = \"Door\"\n)\ndef Xform \"Door\" (kind = \"subcomponent\")\n{\n}\n"));
    PXR_NS::SdfLayerRefPtr house = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    ASSERT_TRUE(house->ImportFromString("#usda 1.0\n(\n    defaultPrim = \"House\"\n)\ndef Xform \"House\" (kind = \"component\")\n{\n"
                                        "    def Xform \"Door\" (\n        prepend payload = @"
                                        + door->GetIdentifier() + "@\n    )\n    {\n    }\n}\n"));
    PXR_NS::UsdStageRefPtr stage = openStage("def Xform \"World\" (kind = \"group\")\n{\n    def Xform \"House\" (\n        prepend payload = @"
                                                 + house->GetIdentifier() + "@\n    )\n    {\n    }\n}\n",
                                             PXR_NS::UsdStage::LoadNone);

    SceneKindWalkResult result = walkToKind(stage, paths({ "/World" }), kSet);
    EXPECT_EQ(std::vector<std::string>(), strings(result.matches));
    EXPECT_EQ(std::vector<std::string>({ "/World/House" }), strings(result.unloadedPassThrough));

    // No natronTestSet anywhere below: the component stops nothing, so a load to the kind would
    // go on into the door's payload, as a load below would.
    stage->Load(PXR_NS::SdfPath("/World/House"), PXR_NS::UsdLoadWithoutDescendants);
    result = walkToKind(stage, paths({ "/World" }), kSet);
    EXPECT_EQ(std::vector<std::string>(), strings(result.matches));
    EXPECT_EQ(std::vector<std::string>({ "/World/House/Door" }), strings(result.unloadedPassThrough));

    result = walkToKind(stage, paths({ "/World" }), PXR_NS::KindTokens->component);
    EXPECT_EQ(std::vector<std::string>({ "/World/House" }), strings(result.matches));
    EXPECT_EQ(std::vector<std::string>(), strings(result.unloadedPassThrough));
}

TEST(SceneKindWalk, NestedRootsAreWalkedOnce)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World/City/Block", "/World/City", "/World/Missing" }), PXR_NS::KindTokens->component);

    EXPECT_EQ(std::vector<std::string>({ "/World/City/Block/HouseA", "/World/City/Block/HouseB" }), strings(result.matches));
}

TEST(SceneKindWalk, ARootOfTheTargetKindIsWalkedThroughToTheNearestBelow)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World/City" }), PXR_NS::KindTokens->assembly);

    EXPECT_EQ(std::vector<std::string>({ "/World/City/Block" }), strings(result.matches));
}

TEST(SceneKindWalk, AWalkerWalksThroughARootOfTheTargetKind)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    SceneKindWalker walker(paths({ "/World/City/Block/HouseA" }), PXR_NS::KindTokens->component);

    EXPECT_TRUE(walker.walk(stage, []() { return false; }, 64));
    // The door is a subcomponent, below the component level: the walk stops there without a match.
    EXPECT_EQ(std::vector<std::string>(), strings(walker.getResult().matches));
}

TEST(SceneKindWalk, InactiveAndAbstractPrimsAreSkipped)
{
    PXR_NS::UsdStageRefPtr stage = openStage(R"(
def Xform "World" (kind = "group")
{
    def Xform "Off" (
        active = false
        kind = "component"
    )
    {
    }
    class Xform "Template" (kind = "component")
    {
    }
    def Xform "On" (kind = "component")
    {
    }
}
)");
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/" }), PXR_NS::KindTokens->component);

    EXPECT_EQ(std::vector<std::string>({ "/World/On" }), strings(result.matches));
}

TEST(SceneKindWalk, UnloadedPayloadsAreReported)
{
    PXR_NS::SdfLayerRefPtr asset = PXR_NS::SdfLayer::CreateAnonymous(".usda");

    ASSERT_TRUE(asset->ImportFromString(R"(#usda 1.0
(
    defaultPrim = "Asset"
)
def Xform "Asset" (kind = "component")
{
    def Xform "Part" (
        prepend payload = @./missing.usda@
    )
    {
    }
}
)"));
    PXR_NS::SdfLayerRefPtr set = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    ASSERT_TRUE(set->ImportFromString("#usda 1.0\n(\n    defaultPrim = \"Set\"\n)\ndef Xform \"Set\" (kind = \"assembly\")\n{\n"
                                      "    def Xform \"House\" (\n        prepend payload = @"
                                      + asset->GetIdentifier() + "@\n    )\n    {\n    }\n}\n"));
    PXR_NS::UsdStageRefPtr stage = openStage("def Xform \"World\" (\n    prepend payload = @" + set->GetIdentifier() + "@\n)\n{\n}\n",
                                             PXR_NS::UsdStage::LoadNone);

    SceneKindWalkResult result = walkToKind(stage, paths({ "/" }), PXR_NS::KindTokens->component);
    EXPECT_EQ(std::vector<std::string>(), strings(result.matches));
    EXPECT_EQ(std::vector<std::string>({ "/World" }), strings(result.unloadedPassThrough));

    stage->Load(PXR_NS::SdfPath("/World"), PXR_NS::UsdLoadWithoutDescendants);
    result = walkToKind(stage, paths({ "/" }), PXR_NS::KindTokens->component);
    EXPECT_EQ(std::vector<std::string>(), strings(result.matches));
    // The house's payload holds its kind: until it loads the walk cannot tell it is a component.
    EXPECT_EQ(std::vector<std::string>({ "/World/House" }), strings(result.unloadedPassThrough));

    stage->Load(PXR_NS::SdfPath("/World/House"), PXR_NS::UsdLoadWithoutDescendants);
    result = walkToKind(stage, paths({ "/" }), PXR_NS::KindTokens->component);
    EXPECT_EQ(std::vector<std::string>({ "/World/House" }), strings(result.matches));
    EXPECT_EQ(std::vector<std::string>(), strings(result.unloadedPassThrough));

    EXPECT_EQ(std::vector<std::string>({ "/World/House/Part" }), strings(payloadsBelow(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/House")))));
    EXPECT_EQ(std::vector<std::string>({ "/World" }), strings(payloadsBelow(stage->GetPseudoRoot())));
}

TEST(SceneKindWalk, CancelStopsTheWalk)
{
    std::string body = "def Xform \"World\"\n{\n";

    for (int i = 0; i < 2000; ++i) {
        body += "    def Xform \"P" + std::to_string(i) + "\" (kind = \"component\")\n    {\n    }\n";
    }
    body += "}\n";
    PXR_NS::UsdStageRefPtr stage = openStage(body);
    int polls = 0;
    const SceneKindWalkResult result = walkToKind(stage, paths({ "/World" }), PXR_NS::KindTokens->component, [&]() {
        return ++polls >= 2;
    });

    EXPECT_TRUE(result.cancelled);
    EXPECT_LT(result.matches.size(), 2000u);
    EXPECT_GT(result.matches.size(), 0u);
}

TEST(SceneKindWalk, AWalkerResumedAtEveryPrimFindsWhatOneWalkDoes)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);

    for (const PXR_NS::TfToken& target : { PXR_NS::KindTokens->assembly, PXR_NS::KindTokens->component, PXR_NS::KindTokens->subcomponent, kTree, kSet }) {
        const SceneKindWalkResult whole = walkToKind(stage, paths({ "/World" }), target);
        SceneKindWalker walker(paths({ "/World" }), target);
        int parts = 0;
        while (!walker.walk(stage, []() { return true; }, 1)) {
            ++parts;
            ASSERT_LT(parts, 1000);
        }
        EXPECT_TRUE(walker.isDone());
        EXPECT_GT(parts, 1) << target.GetString();
        EXPECT_EQ(strings(whole.matches), strings(walker.getResult().matches)) << target.GetString();
        EXPECT_EQ(strings(whole.unloadedPassThrough), strings(walker.getResult().unloadedPassThrough)) << target.GetString();
    }
}

TEST(SceneKindWalk, AWalkerStopsPartwayThroughAWideChildList)
{
    std::string body = "def Xform \"World\"\n{\n";

    for (int i = 0; i < 2000; ++i) {
        body += "    def Xform \"P" + std::to_string(i) + "\" (kind = \"component\")\n    {\n    }\n";
    }
    body += "}\n";
    PXR_NS::UsdStageRefPtr stage = openStage(body);
    SceneKindWalker walker(paths({ "/World" }), PXR_NS::KindTokens->component);
    std::size_t parts = 0;
    std::size_t visited = 0;
    bool done = false;

    while (!done) {
        done = walker.walk(stage, []() { return true; }, 16);
        EXPECT_LE(walker.getVisitedCount() - visited, 16u);
        visited = walker.getVisitedCount();
        ++parts;
        ASSERT_LT(parts, 1000u);
    }
    EXPECT_GT(parts, 100u);
    EXPECT_EQ(2001u, walker.getVisitedCount());
    ASSERT_EQ(2000u, walker.getResult().matches.size());
    EXPECT_EQ(std::string("/World/P0"), walker.getResult().matches.front().GetString());
    EXPECT_EQ(std::string("/World/P1999"), walker.getResult().matches.back().GetString());
}

TEST(SceneKindWalk, APrimGoneBetweenPartsIsPassedOver)
{
    PXR_NS::UsdStageRefPtr stage = openStage(kWorld);
    SceneKindWalker walker(paths({ "/World" }), PXR_NS::KindTokens->component);

    ASSERT_FALSE(walker.walk(stage, []() { return true; }, 1));
    stage->RemovePrim(PXR_NS::SdfPath("/World/Props"));
    while (!walker.walk(stage, std::function<bool()>(), 1)) {
    }
    EXPECT_EQ(std::vector<std::string>({ "/World/City/Block/HouseA", "/World/City/Block/HouseB", "/World/Park/Grove/Oak", "/World/Park/Bench" }),
              strings(walker.getResult().matches));
}

TEST(SceneKindWalk, InstancesAreWalkedThroughTheirProxies)
{
    PXR_NS::UsdStageRefPtr stage = openStage(R"(
def Xform "Proto" (kind = "assembly")
{
    def Xform "House" (kind = "component")
    {
        def Xform "Door" (kind = "subcomponent")
        {
        }
    }
}
def Xform "World"
{
    def Xform "SetA" (
        instanceable = true
        prepend references = </Proto>
    )
    {
    }
    def Xform "SetB" (
        instanceable = true
        prepend references = </Proto>
    )
    {
    }
}
)");
    ASSERT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/SetA")).IsInstance());

    SceneKindWalkResult result = walkToKind(stage, paths({ "/World" }), PXR_NS::KindTokens->component);
    EXPECT_EQ(std::vector<std::string>({ "/World/SetA/House", "/World/SetB/House" }), strings(result.matches));

    result = walkToKind(stage, paths({ "/World/SetA/House" }), PXR_NS::KindTokens->subcomponent);
    EXPECT_EQ(std::vector<std::string>({ "/World/SetA/House/Door" }), strings(result.matches));

    result = walkToKind(stage, paths({ "/World" }), PXR_NS::KindTokens->assembly);
    EXPECT_EQ(std::vector<std::string>({ "/World/SetA", "/World/SetB" }), strings(result.matches));
}
