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
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
// /World holds an island assembly whose payload holds a group of two trees, a rock and a bush,
// each a payload, and a crate component under an unkinded scope. A tree's kind, component, is in
// its payload; the rock's is authored where it is referenced. Each component holds a nested
// subcomponent payload. The bush is natronTestTree, which Tests/fixtures/sceneKinds registers.
const char* const kWorld = NATRON_TESTS_FIXTURES_DIR "/sceneKindPayloads/world.usda";

const SceneStage::LockedSection kNoLock = [](const std::function<void()>& section) { section(); };

ScenePayload
worldPayload()
{
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(kWorld);

    EXPECT_TRUE(bool(layer));

    return SceneOps::appendLayer(ScenePayload(), layer, 1);
}

std::vector<std::string>
loaded(const SceneStage& stage)
{
    std::vector<std::string> paths;

    for (const PXR_NS::SdfPath& path : stage.getStage()->GetLoadSet()) {
        paths.push_back(path.GetString());
    }
    std::sort(paths.begin(), paths.end());

    return paths;
}

// Services the stage until nothing is pending; returns how many batches it took.
int
serviceAll(SceneStage& stage)
{
    int batches = 0;

    while (stage.serviceLoads(kNoLock, kNoLock) && (batches < 32)) {
        ++batches;
    }

    return batches;
}

const PXR_NS::SdfPathVector kRoot = { PXR_NS::SdfPath("/World") };
} // anonymous namespace

TEST(SceneStageLoadToKind, ComponentLoadsAssembliesAndComponentsButNotTheirSubcomponents)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));

    stage.requestLoadToKind(kRoot, PXR_NS::KindTokens->component);
    EXPECT_TRUE(stage.hasPendingLoads());
    // One batch loads the island and the crate, the next the components the island reveals, and a
    // third finds nothing more.
    EXPECT_EQ(3, serviceAll(stage));
    EXPECT_FALSE(stage.hasPendingLoads());
    EXPECT_FALSE(stage.hasOutstandingLoads());

    EXPECT_EQ(std::vector<std::string>({ "/World/Island", "/World/Island/Bush", "/World/Island/Grove/TreeA", "/World/Island/Grove/TreeB",
                                         "/World/Island/Rock", "/World/Props/Crate" }),
              loaded(stage));
}

TEST(SceneStageLoadToKind, AssemblyStopsAtTheAssembly)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));

    stage.requestLoadToKind(kRoot, PXR_NS::KindTokens->assembly);
    serviceAll(stage);

    EXPECT_EQ(std::vector<std::string>({ "/World/Island" }), loaded(stage));
}

TEST(SceneStageLoadToKind, SubcomponentDescendsThroughComponents)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));

    stage.requestLoadToKind(kRoot, PXR_NS::KindTokens->subcomponent);
    serviceAll(stage);

    EXPECT_EQ(std::vector<std::string>({ "/World/Island", "/World/Island/Bush", "/World/Island/Bush/Berries", "/World/Island/Grove/TreeA",
                                         "/World/Island/Grove/TreeA/Leaves", "/World/Island/Grove/TreeB", "/World/Island/Grove/TreeB/Leaves",
                                         "/World/Island/Rock", "/World/Island/Rock/Moss", "/World/Props/Crate", "/World/Props/Crate/Lid" }),
              loaded(stage));
}

TEST(SceneStageLoadToKind, CustomKindLoadsOnlyWhatItCannotTellApart)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));

    stage.requestLoadToKind(kRoot, PXR_NS::TfToken("natronTestTree"));
    serviceAll(stage);

    // The trees' kind is in their payloads, so they load to be told apart; the rock and the crate
    // say they are plain components where they are referenced, and stay unloaded.
    EXPECT_EQ(std::vector<std::string>({ "/World/Island", "/World/Island/Bush", "/World/Island/Grove/TreeA", "/World/Island/Grove/TreeB" }),
              loaded(stage));
}

TEST(SceneStageLoadToKind, RootInsideTheSceneLoadsOnlyItsSubtree)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));

    stage.requestLoadToKind({ PXR_NS::SdfPath("/World/Props") }, PXR_NS::KindTokens->component);
    serviceAll(stage);

    EXPECT_EQ(std::vector<std::string>({ "/World/Props/Crate" }), loaded(stage));
}

TEST(SceneStageLoadToKind, UnloadBelowComponentLeavesComponentsLoaded)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));
    stage.requestLoad(kRoot, true);
    serviceAll(stage);
    ASSERT_EQ(11u, loaded(stage).size());

    stage.requestUnloadBelowKind(kRoot, PXR_NS::KindTokens->component);
    EXPECT_TRUE(stage.hasPendingLoads());
    EXPECT_EQ(1, serviceAll(stage));

    EXPECT_EQ(std::vector<std::string>({ "/World/Island", "/World/Island/Bush", "/World/Island/Grove/TreeA", "/World/Island/Grove/TreeB",
                                         "/World/Island/Rock", "/World/Props/Crate" }),
              loaded(stage));
}

TEST(SceneStageLoadToKind, UnloadBelowAssemblyKeepsTheAssembly)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));
    stage.requestLoad(kRoot, true);
    serviceAll(stage);

    stage.requestUnloadBelowKind(kRoot, PXR_NS::KindTokens->assembly);
    serviceAll(stage);

    // The crate is under no assembly, so it is not below one.
    EXPECT_EQ(std::vector<std::string>({ "/World/Island", "/World/Props/Crate", "/World/Props/Crate/Lid" }), loaded(stage));
}

TEST(SceneStageLoadToKind, CancelEndsTheWalkWithAConsistentLoadSet)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));

    stage.requestLoadToKind(kRoot, PXR_NS::KindTokens->component);
    EXPECT_TRUE(stage.serviceLoads(kNoLock, kNoLock));
    EXPECT_TRUE(stage.hasPendingLoads());
    stage.cancelPendingLoads();

    EXPECT_FALSE(stage.hasPendingLoads());
    EXPECT_FALSE(stage.serviceLoads(kNoLock, kNoLock));
    EXPECT_EQ(std::vector<std::string>({ "/World/Island", "/World/Props/Crate" }), loaded(stage));
    for (const std::string& path : loaded(stage)) {
        EXPECT_TRUE(stage.getStage()->GetPrimAtPath(PXR_NS::SdfPath(path)).IsLoaded()) << path;
    }
}

TEST(SceneStageLoadToKind, CancelAtTheRootEndsItsWalkOnly)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));

    stage.requestLoadToKind(kRoot, PXR_NS::KindTokens->component);
    stage.requestLoadToKind({ PXR_NS::SdfPath("/World/Props") }, PXR_NS::KindTokens->component);
    stage.cancelPendingLoads(kRoot);
    serviceAll(stage);

    EXPECT_EQ(std::vector<std::string>({ "/World/Props/Crate" }), loaded(stage));
}

TEST(SceneStageLoadToKind, CancelWhileServicingStopsTheNextLevel)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));

    stage.requestLoadToKind(kRoot, PXR_NS::KindTokens->component);
    const SceneStage::LockedSection cancellingWrite = [&](const std::function<void()>& section) {
        stage.cancelPendingLoads();
        section();
    };
    EXPECT_TRUE(stage.serviceLoads(kNoLock, cancellingWrite));

    // The batch taken commits; the walk does not ask for the level below it.
    EXPECT_FALSE(stage.hasPendingLoads());
    EXPECT_EQ(std::vector<std::string>({ "/World/Island", "/World/Props/Crate" }), loaded(stage));
}

TEST(SceneStageLoadToKind, MissingPayloadIsAskedForOnce)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneStageLoadToKindTest");

    ASSERT_TRUE(layer->ImportFromString(R"(#usda 1.0
def Xform "World"
{
    def Xform "Gone" (
        prepend payload = @/nonexistent/natron/missing_asset.usda@
    )
    {
    }
}
)"));
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(SceneOps::appendLayer(ScenePayload(), layer, 1)));

    stage.requestLoadToKind(kRoot, PXR_NS::KindTokens->component);
    EXPECT_LE(serviceAll(stage), 2);
    EXPECT_FALSE(stage.hasPendingLoads());
    EXPECT_FALSE(stage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Gone")).IsLoaded());
}

TEST(SceneStageLoadToKind, LaterRequestAtARootReplacesTheEarlier)
{
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(worldPayload()));

    stage.requestLoadToKind(kRoot, PXR_NS::KindTokens->subcomponent);
    stage.requestLoadToKind(kRoot, PXR_NS::KindTokens->assembly);
    serviceAll(stage);

    EXPECT_EQ(std::vector<std::string>({ "/World/Island" }), loaded(stage));
}
