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
#include <latch>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneDrawFilter.h"

#ifdef emit
#define NATRON_SCENEDRAWFILTER_TEST_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/unitTestNullRenderDelegate.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#ifdef NATRON_SCENEDRAWFILTER_TEST_RESTORE_EMIT
#undef NATRON_SCENEDRAWFILTER_TEST_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {
using PathSet = std::set<std::string>;

bool
visible(const HdSceneIndexPrim& prim)
{
    const HdBoolDataSourceHandle visibility = HdVisibilitySchema::GetFromParent(prim.dataSource).GetVisibility();

    return !visibility || visibility->GetTypedValue(0.f);
}

// hides and shows hold every prim that stopped or started being drawn; leftHydra and invisible say
// which of the hides took the prim out of Hydra and which kept it there, invisible.
class Recorder
    : public HdSceneIndexObserver {
public:
    void PrimsAdded(const HdSceneIndexBase& /*sender*/,
                    const AddedPrimEntries& entries) override
    {
        for (const AddedPrimEntry& entry : entries) {
            if (entry.primType.IsEmpty()) {
                hides.insert(entry.primPath.GetString());
                leftHydra.insert(entry.primPath.GetString());
            } else {
                shows.insert(entry.primPath.GetString());
            }
        }
    }

    void PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                      const RemovedPrimEntries& entries) override
    {
        for (const RemovedPrimEntry& entry : entries) {
            removed.insert(entry.primPath.GetString());
        }
    }

    void PrimsDirtied(const HdSceneIndexBase& sender,
                      const DirtiedPrimEntries& entries) override
    {
        static const HdDataSourceLocatorSet visibilityOnly { HdVisibilitySchema::GetDefaultLocator() };
        for (const DirtiedPrimEntry& entry : entries) {
            if (entry.dirtyLocators != visibilityOnly) {
                dirtied.insert(entry.primPath.GetString());
            } else if (visible(sender.GetPrim(entry.primPath))) {
                shows.insert(entry.primPath.GetString());
            } else {
                hides.insert(entry.primPath.GetString());
                invisible.insert(entry.primPath.GetString());
            }
        }
    }

    void PrimsRenamed(const HdSceneIndexBase& sender,
                      const RenamedPrimEntries& entries) override
    {
        ConvertPrimsRenamedToRemovedAndAdded(sender, entries, this);
    }

    void reset()
    {
        hides.clear();
        shows.clear();
        leftHydra.clear();
        invisible.clear();
        removed.clear();
        dirtied.clear();
    }

    bool empty() const
    {
        return hides.empty() && shows.empty() && removed.empty() && dirtied.empty();
    }

    PathSet hides;
    PathSet shows;
    PathSet leftHydra;
    PathSet invisible;
    PathSet removed;
    PathSet dirtied;
};

HdContainerDataSourceHandle
someData()
{
    return HdRetainedContainerDataSource::New(TfToken("marker"), HdRetainedTypedSampledDataSource<int>::New(1));
}

// /World
//   Looks (scope), Looks/mat (material)
//   cam (camera)
//   a (xform), a/m1 (mesh), a/Looks/mat (material), a/deep (xform), a/deep/m2 (mesh)
//   b (xform), b/m3 (mesh), b/ball (sphere)
//   inst (instancer)
//   c (mesh)
HdRetainedSceneIndexRefPtr
makeScene()
{
    HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();
    const TfToken xform("xform");
    const auto add = [&scene](const char* path, const TfToken& type) {
        scene->AddPrims({ { SdfPath(path), type, someData() } });
    };
    add("/World", xform);
    add("/World/Looks", TfToken("scope"));
    add("/World/Looks/mat", HdPrimTypeTokens->material);
    add("/World/cam", HdPrimTypeTokens->camera);
    add("/World/a", xform);
    add("/World/a/m1", HdPrimTypeTokens->mesh);
    add("/World/a/Looks", TfToken("scope"));
    add("/World/a/Looks/mat", HdPrimTypeTokens->material);
    add("/World/a/deep", xform);
    add("/World/a/deep/m2", HdPrimTypeTokens->mesh);
    add("/World/b", xform);
    add("/World/b/m3", HdPrimTypeTokens->mesh);
    add("/World/b/ball", HdPrimTypeTokens->sphere);
    add("/World/inst", HdPrimTypeTokens->instancer);
    add("/World/c", HdPrimTypeTokens->mesh);

    return scene;
}

const PathSet kAllGeometry = { "/World/a/m1", "/World/a/deep/m2", "/World/b/m3", "/World/b/ball", "/World/inst", "/World/c" };

// /World/rig (xform), rig/key (a light row), rig/key/fill (light below a light), rig/dome (domeLight),
// rig/subrig/spot (sphereLight), rig/mesh (mesh)
void
addLights(const HdRetainedSceneIndexRefPtr& scene)
{
    const auto add = [&scene](const char* path, const TfToken& type) {
        scene->AddPrims({ { SdfPath(path), type, someData() } });
    };
    add("/World/rig", TfToken("xform"));
    add("/World/rig/key", HdPrimTypeTokens->rectLight);
    add("/World/rig/key/fill", HdPrimTypeTokens->distantLight);
    add("/World/rig/dome", HdPrimTypeTokens->domeLight);
    add("/World/rig/subrig", TfToken("xform"));
    add("/World/rig/subrig/spot", HdPrimTypeTokens->sphereLight);
    add("/World/rig/mesh", HdPrimTypeTokens->mesh);
}

// A native instancer as usdImaging propagates it, away from the instances it names.
HdContainerDataSourceHandle
instancerData(const std::vector<std::string>& locations,
              const std::vector<bool>& mask = std::vector<bool>())
{
    VtArray<SdfPath> paths;
    for (const std::string& location : locations) {
        paths.push_back(SdfPath(location));
    }
    HdInstancerTopologySchema::Builder builder;
    builder.SetInstanceLocations(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(paths));
    if (!mask.empty()) {
        builder.SetMask(HdRetainedTypedSampledDataSource<VtArray<bool>>::New(VtArray<bool>(mask.begin(), mask.end())));
    }

    return HdRetainedContainerDataSource::New(HdInstancerTopologySchema::GetSchemaToken(), builder.Build());
}

const char* const kNativeInstancer = "/UsdNiPropagatedPrototypes/Bindings/__Prototype_1/UsdNiInstancer";

// /World/a/i1, /World/a/i2 and /World/b/i3, drawn by kNativeInstancer.
void
addInstances(const HdRetainedSceneIndexRefPtr& scene,
             const std::vector<bool>& mask = std::vector<bool>())
{
    const auto add = [&scene](const char* path, const TfToken& type, const HdContainerDataSourceHandle& data) {
        scene->AddPrims({ { SdfPath(path), type, data } });
    };
    add("/World/a/i1", TfToken(), someData());
    add("/World/a/i2", TfToken(), someData());
    add("/World/b/i3", TfToken(), someData());
    add("/UsdNiPropagatedPrototypes", TfToken(), someData());
    add("/UsdNiPropagatedPrototypes/Bindings", TfToken(), someData());
    add("/UsdNiPropagatedPrototypes/Bindings/__Prototype_1", TfToken(), someData());
    add(kNativeInstancer, HdPrimTypeTokens->instancer, instancerData({ "/World/a/i1", "/World/a/i2", "/World/b/i3" }, mask));
    add("/UsdNiPropagatedPrototypes/Bindings/__Prototype_1/UsdNiInstancer/UsdNiPrototype", TfToken("xform"), someData());
    add("/UsdNiPropagatedPrototypes/Bindings/__Prototype_1/UsdNiInstancer/UsdNiPrototype/m", HdPrimTypeTokens->mesh, someData());
}

// Empty when the instancer draws every instance.
std::vector<bool>
maskOf(const HdSceneIndexBase& index,
       const char* path)
{
    const HdSceneIndexPrim prim = index.GetPrim(SdfPath(path));
    const HdBoolArrayDataSourceHandle mask = HdInstancerTopologySchema::GetFromParent(prim.dataSource).GetMask();
    if (!mask) {
        return std::vector<bool>();
    }
    const VtArray<bool> values = mask->GetTypedValue(0.f);
    std::vector<bool> result(values.cbegin(), values.cend());
    if (std::find(result.begin(), result.end(), false) == result.end()) {
        result.clear();
    }

    return result;
}

class NullLight
    : public HdSprim {
public:
    explicit NullLight(const SdfPath& id)
        : HdSprim(id)
    {
    }

    void Sync(HdSceneDelegate* /*sceneDelegate*/,
              HdRenderParam* /*renderParam*/,
              HdDirtyBits* dirtyBits) override
    {
        *dirtyBits = HdChangeTracker::Clean;
    }

    HdDirtyBits GetInitialDirtyBitsMask() const override
    {
        return HdChangeTracker::AllDirty;
    }
};

// Hd_UnitTestNullRenderDelegate is final and makes no lights, so this forwards to one and adds them.
class CountingRenderDelegate
    : public HdRenderDelegate {
public:
    const TfTokenVector& GetSupportedRprimTypes() const override { return _null.GetSupportedRprimTypes(); }
    const TfTokenVector& GetSupportedBprimTypes() const override { return _null.GetSupportedBprimTypes(); }
    HdResourceRegistrySharedPtr GetResourceRegistry() const override { return _null.GetResourceRegistry(); }

    const TfTokenVector& GetSupportedSprimTypes() const override
    {
        static const TfTokenVector types = [this]() {
            TfTokenVector all = _null.GetSupportedSprimTypes();
            all.push_back(HdPrimTypeTokens->rectLight);
            all.push_back(HdPrimTypeTokens->domeLight);

            return all;
        }();

        return types;
    }

    HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex* index,
                                           const HdRprimCollection& collection) override
    {
        return _null.CreateRenderPass(index, collection);
    }

    HdInstancer* CreateInstancer(HdSceneDelegate* delegate,
                                 const SdfPath& id) override
    {
        return _null.CreateInstancer(delegate, id);
    }

    void DestroyInstancer(HdInstancer* instancer) override { _null.DestroyInstancer(instancer); }

    HdRprim* CreateRprim(const TfToken& typeId,
                         const SdfPath& id) override
    {
        return _null.CreateRprim(typeId, id);
    }

    void DestroyRprim(HdRprim* rprim) override
    {
        ++rprimsDestroyed;
        _null.DestroyRprim(rprim);
    }

    HdSprim* CreateSprim(const TfToken& typeId,
                         const SdfPath& id) override
    {
        return HdPrimTypeIsLight(typeId) ? new NullLight(id) : _null.CreateSprim(typeId, id);
    }

    HdSprim* CreateFallbackSprim(const TfToken& typeId) override
    {
        return HdPrimTypeIsLight(typeId) ? new NullLight(SdfPath::EmptyPath()) : _null.CreateFallbackSprim(typeId);
    }

    void DestroySprim(HdSprim* sprim) override
    {
        if (dynamic_cast<NullLight*>(sprim)) {
            delete sprim;
        } else {
            _null.DestroySprim(sprim);
        }
    }

    HdBprim* CreateBprim(const TfToken& typeId,
                         const SdfPath& id) override
    {
        return _null.CreateBprim(typeId, id);
    }

    HdBprim* CreateFallbackBprim(const TfToken& typeId) override { return _null.CreateFallbackBprim(typeId); }
    void DestroyBprim(HdBprim* bprim) override { _null.DestroyBprim(bprim); }
    void CommitResources(HdChangeTracker* tracker) override { _null.CommitResources(tracker); }

    int rprimsDestroyed = 0;

private:
    Hd_UnitTestNullRenderDelegate _null;
};

class SceneDrawFilterTest
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        scene = makeScene();
        filter = SceneDrawFilter::New(scene);
        filter->AddObserver(HdSceneIndexObserverPtr(&recorder));
    }

    bool drawn(const char* path) const
    {
        const HdSceneIndexPrim prim = filter->GetPrim(SdfPath(path));

        return !prim.primType.IsEmpty() && visible(prim);
    }

    PathSet drawnGeometry() const
    {
        PathSet result;
        for (const std::string& path : kAllGeometry) {
            if (drawn(path.c_str())) {
                result.insert(path);
            }
        }

        return result;
    }

    HdRetainedSceneIndexRefPtr scene;
    TfRefPtr<SceneDrawFilter> filter;
    Recorder recorder;
    SceneDrawStates states;
};
} // anon namespace

TEST_F(SceneDrawFilterTest, NoRulesPassEverythingThrough)
{
    EXPECT_EQ(kAllGeometry, drawnGeometry());
    EXPECT_TRUE(recorder.hides.empty());
    EXPECT_EQ(HdPrimTypeTokens->mesh, filter->GetPrim(SdfPath("/World/c")).primType);
    EXPECT_TRUE(filter->GetPrim(SdfPath("/World/c")).dataSource != nullptr);
}

TEST_F(SceneDrawFilterTest, PruneHidesOnlyRprimsAndInstancersBelow)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));

    EXPECT_EQ(kAllGeometry, recorder.hides);
    EXPECT_TRUE(recorder.removed.empty());
    EXPECT_TRUE(drawnGeometry().empty());
    EXPECT_EQ(HdPrimTypeTokens->material, filter->GetPrim(SdfPath("/World/Looks/mat")).primType);
    EXPECT_EQ(HdPrimTypeTokens->material, filter->GetPrim(SdfPath("/World/a/Looks/mat")).primType);
    EXPECT_EQ(HdPrimTypeTokens->camera, filter->GetPrim(SdfPath("/World/cam")).primType);
    EXPECT_TRUE(visible(filter->GetPrim(SdfPath("/World/cam"))));
}

TEST_F(SceneDrawFilterTest, BoxingKeepsPrimsInHydraInvisibleWithTheirData)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), recorder.invisible);
    EXPECT_TRUE(recorder.leftHydra.empty());
    const HdSceneIndexPrim boxed = filter->GetPrim(SdfPath("/World/a/m1"));
    EXPECT_EQ(HdPrimTypeTokens->mesh, boxed.primType);
    EXPECT_FALSE(visible(boxed));
    ASSERT_TRUE(boxed.dataSource != nullptr);
    EXPECT_TRUE(boxed.dataSource->Get(TfToken("marker")) != nullptr);
    EXPECT_EQ(2u, filter->getHeldInvisibleCount());
}

TEST_F(SceneDrawFilterTest, PruneKeepsTheHierarchy)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));

    EXPECT_EQ(scene->GetChildPrimPaths(SdfPath("/World")), filter->GetChildPrimPaths(SdfPath("/World")));
    EXPECT_EQ(scene->GetChildPrimPaths(SdfPath("/World/a")), filter->GetChildPrimPaths(SdfPath("/World/a")));
}

TEST_F(SceneDrawFilterTest, HiddenTakesPrimsOutOfHydraWhereBBoxKeepsThem)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateHidden));

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), recorder.leftHydra);
    EXPECT_TRUE(recorder.invisible.empty());
    const HdSceneIndexPrim hidden = filter->GetPrim(SdfPath("/World/a/m1"));
    EXPECT_TRUE(hidden.primType.IsEmpty());
    EXPECT_TRUE(hidden.dataSource == nullptr);
    EXPECT_EQ(0u, filter->getHeldInvisibleCount());
}

TEST_F(SceneDrawFilterTest, HidingABoxedRowTakesItsPrimsOutAndBoxingItAgainLeavesThemOut)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    recorder.reset();

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateHidden));

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), recorder.leftHydra);
    EXPECT_TRUE(recorder.shows.empty());
    EXPECT_EQ(0u, filter->getHeldInvisibleCount());
    recorder.reset();

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));

    EXPECT_TRUE(recorder.empty());
    EXPECT_TRUE(filter->GetPrim(SdfPath("/World/a/m1")).primType.IsEmpty());

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull));

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), recorder.shows);
    EXPECT_EQ(kAllGeometry, drawnGeometry());
}

TEST_F(SceneDrawFilterTest, ChangesAppliedTogetherThatCancelOutSendNothing)
{
    const SceneDrawDelta boxed = states.setState("/World/a", eSceneDrawStateBBox);
    const SceneDrawDelta full = states.setState("/World/a", eSceneDrawStateFull);
    const unsigned long long sentBefore = filter->getHiddenSentCount() + filter->getShownSentCount();

    filter->applyDeltas({ &boxed, &full });

    EXPECT_TRUE(recorder.empty());
    EXPECT_EQ(sentBefore, filter->getHiddenSentCount() + filter->getShownSentCount());
    EXPECT_EQ(kAllGeometry, drawnGeometry());
    EXPECT_TRUE(filter->getRules().empty());
}

TEST_F(SceneDrawFilterTest, ChangesAppliedTogetherAnnounceEachPrimOnce)
{
    const SceneDrawDelta boxed = states.setState("/World/a", eSceneDrawStateBBox);
    const SceneDrawDelta full = states.setState("/World/a", eSceneDrawStateFull);
    const SceneDrawDelta deepHidden = states.setState("/World/a/deep", eSceneDrawStateHidden);

    filter->applyDeltas({ &boxed, &full, &deepHidden });

    EXPECT_EQ((PathSet { "/World/a/deep/m2" }), recorder.leftHydra);
    EXPECT_TRUE(recorder.invisible.empty());
    EXPECT_TRUE(recorder.shows.empty());
    EXPECT_EQ(1u, filter->getHiddenSentCount());
    EXPECT_EQ(0u, filter->getShownSentCount());
    EXPECT_TRUE(drawn("/World/a/m1"));
}

TEST_F(SceneDrawFilterTest, RestoreAnnouncesWhatWasHiddenWithItsType)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    recorder.reset();

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull));

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), recorder.shows);
    EXPECT_TRUE(recorder.hides.empty());
    EXPECT_EQ(kAllGeometry, drawnGeometry());
    EXPECT_FALSE(filter->isRevealing());
    EXPECT_TRUE(filter->getRules().empty());
}

TEST_F(SceneDrawFilterTest, SiblingsDoNotHearAboutEachOther)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    recorder.reset();

    filter->applyDelta(states.setState("/World/b", eSceneDrawStateBBox));

    EXPECT_EQ((PathSet { "/World/b/m3", "/World/b/ball" }), recorder.hides);
    EXPECT_TRUE(recorder.shows.empty());
}

TEST_F(SceneDrawFilterTest, ChildrenRowKeepsItsOwnPrimAndHidesTheRest)
{
    filter->applyDelta(states.setState("/World/inst", eSceneDrawStateChildren));

    EXPECT_TRUE(recorder.hides.empty());
    EXPECT_TRUE(drawn("/World/inst"));

    filter->applyDelta(states.setState("/World", eSceneDrawStateChildren));

    EXPECT_EQ((PathSet { "/World/inst" }), drawnGeometry());
}

TEST_F(SceneDrawFilterTest, ExceptionBelowAPrunedPrefix)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    recorder.reset();

    filter->applyDelta(states.setState("/World/a/m1", eSceneDrawStateFull));

    EXPECT_EQ((PathSet { "/World/a/m1" }), recorder.shows);
    EXPECT_TRUE(recorder.hides.empty());
    EXPECT_EQ((PathSet { "/World/a/m1" }), drawnGeometry());
}

TEST_F(SceneDrawFilterTest, NestedExceptionsAndABoxWithinThem)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull));
    recorder.reset();
    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), drawnGeometry());

    filter->applyDelta(states.setState("/World/a/deep", eSceneDrawStateBBox));
    EXPECT_EQ((PathSet { "/World/a/deep/m2" }), recorder.hides);
    EXPECT_EQ((PathSet { "/World/a/m1" }), drawnGeometry());
    recorder.reset();

    filter->applyDelta(states.setState("/World/a/deep/m2", eSceneDrawStateFull));
    EXPECT_EQ((PathSet { "/World/a/deep/m2" }), recorder.shows);
    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), drawnGeometry());
}

TEST_F(SceneDrawFilterTest, RememberedRowsComeBackWithTheirAncestor)
{
    filter->applyDelta(states.setState("/World/a/deep", eSceneDrawStateHidden));
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    recorder.reset();

    filter->applyDelta(states.setState("/World", eSceneDrawStateFull));

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/b/m3", "/World/b/ball", "/World/inst", "/World/c" }), recorder.shows);
    EXPECT_FALSE(drawn("/World/a/deep/m2"));
}

TEST_F(SceneDrawFilterTest, ClearingARowReturnsItToTheRuleAbove)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateChildren));
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull));
    recorder.reset();

    filter->applyDelta(states.clearState("/World/a"));

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), recorder.hides);
    EXPECT_TRUE(recorder.shows.empty());
}

TEST_F(SceneDrawFilterTest, ResetRestoresEverything)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateChildren));
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull));
    filter->applyDelta(states.setState("/World/a/deep", eSceneDrawStateHidden));

    filter->applyDelta(states.clearAllStates());

    EXPECT_EQ(kAllGeometry, drawnGeometry());
    EXPECT_TRUE(filter->getRules().empty());
}

TEST_F(SceneDrawFilterTest, MissingPathsAreKeptAndAnnounceNothing)
{
    filter->applyDelta(states.setState("/World/notHere", eSceneDrawStateBBox));

    EXPECT_TRUE(recorder.hides.empty());
    EXPECT_EQ(1u, filter->getRules().size());
    recorder.reset();

    scene->AddPrims({ { SdfPath("/World/notHere"), HdPrimTypeTokens->mesh, someData() } });

    EXPECT_EQ((PathSet { "/World/notHere" }), recorder.hides);
    EXPECT_FALSE(drawn("/World/notHere"));
}

TEST_F(SceneDrawFilterTest, InputNoticesBelowARuleAreRewritten)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    recorder.reset();

    scene->AddPrims({ { SdfPath("/World/a/loaded"), HdPrimTypeTokens->mesh, someData() } });
    scene->AddPrims({ { SdfPath("/World/a/loadedMat"), HdPrimTypeTokens->material, someData() } });
    scene->AddPrims({ { SdfPath("/World/b/loaded"), HdPrimTypeTokens->mesh, someData() } });

    EXPECT_EQ((PathSet { "/World/a/loaded" }), recorder.hides);
    EXPECT_EQ((PathSet { "/World/a/loadedMat", "/World/b/loaded" }), recorder.shows);
}

TEST_F(SceneDrawFilterTest, DirtyingABoxedPrimReachesHydraAndAHiddenOneIsDropped)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World/b", eSceneDrawStateHidden));
    recorder.reset();
    const HdDataSourceLocatorSet all { HdDataSourceLocator::EmptyLocator() };

    scene->DirtyPrims({ { SdfPath("/World/a/m1"), all }, { SdfPath("/World/a/Looks/mat"), all }, { SdfPath("/World/b/m3"), all } });

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/Looks/mat" }), recorder.dirtied);
}

TEST_F(SceneDrawFilterTest, RemovalsPassThrough)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    recorder.reset();

    scene->RemovePrims({ { SdfPath("/World/a") } });

    EXPECT_EQ((PathSet { "/World/a" }), recorder.removed);
}

TEST_F(SceneDrawFilterTest, TwoFiltersOnOneSceneKeepSeparateStates)
{
    TfRefPtr<SceneDrawFilter> other = SceneDrawFilter::New(scene);
    Recorder otherRecorder;
    other->AddObserver(HdSceneIndexObserverPtr(&otherRecorder));
    SceneDrawStates otherStates;

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    other->applyDelta(otherStates.setState("/World/b", eSceneDrawStateHidden));

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), recorder.hides);
    EXPECT_EQ((PathSet { "/World/b/m3", "/World/b/ball" }), otherRecorder.hides);
    EXPECT_FALSE(drawn("/World/a/m1"));
    EXPECT_TRUE(drawn("/World/b/m3"));
    EXPECT_TRUE(!other->GetPrim(SdfPath("/World/a/m1")).primType.IsEmpty());
    EXPECT_TRUE(other->GetPrim(SdfPath("/World/b/m3")).primType.IsEmpty());
}

TEST_F(SceneDrawFilterTest, ARevealAnnouncesInBatchesAndFinishes)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    recorder.reset();

    filter->applyDelta(states.setState("/World", eSceneDrawStateFull), 2);

    EXPECT_TRUE(filter->isRevealing());
    EXPECT_TRUE(recorder.shows.empty());
    EXPECT_TRUE(drawnGeometry().empty());

    EXPECT_EQ(2u, filter->stepReveal(2));
    EXPECT_EQ(2u, drawnGeometry().size());
    EXPECT_EQ(recorder.shows, drawnGeometry());

    while (filter->isRevealing()) {
        filter->stepReveal(2);
    }
    EXPECT_EQ(kAllGeometry, drawnGeometry());
    EXPECT_EQ(kAllGeometry, recorder.shows);
}

TEST_F(SceneDrawFilterTest, ChangingTheStateStopsAReveal)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World", eSceneDrawStateFull), 2);
    filter->stepReveal(2);
    const PathSet announced = recorder.shows;
    recorder.reset();

    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));

    EXPECT_FALSE(filter->isRevealing());
    EXPECT_EQ(announced, recorder.hides);
    EXPECT_TRUE(recorder.shows.empty());
    EXPECT_TRUE(drawnGeometry().empty());
}

TEST_F(SceneDrawFilterTest, APrimAddedDuringARevealIsNotLost)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull), 1);
    filter->stepReveal(1);
    recorder.reset();

    scene->AddPrims({ { SdfPath("/World/a/loaded"), HdPrimTypeTokens->mesh, someData() } });

    EXPECT_EQ((PathSet { "/World/a/loaded" }), recorder.shows);
    while (filter->isRevealing()) {
        filter->stepReveal(1);
    }
    EXPECT_TRUE(drawn("/World/a/loaded"));
    EXPECT_EQ(kAllGeometry, drawnGeometry());
}

TEST_F(SceneDrawFilterTest, SeededRulesApplyWithoutNotices)
{
    SceneDrawFilter::Rules rules;
    SceneDrawFilter::applyToRules(states.setState("/World/a", eSceneDrawStateBBox), &rules);

    TfRefPtr<SceneDrawFilter> fresh = SceneDrawFilter::New(scene);
    Recorder freshRecorder;
    fresh->AddObserver(HdSceneIndexObserverPtr(&freshRecorder));
    fresh->seedRules(rules);

    EXPECT_TRUE(freshRecorder.hides.empty());
    EXPECT_TRUE(fresh->GetPrim(SdfPath("/World/a/m1")).primType.IsEmpty());
    EXPECT_FALSE(fresh->GetPrim(SdfPath("/World/b/m3")).primType.IsEmpty());
}

TEST_F(SceneDrawFilterTest, LightsKeepLightingInBBox)
{
    addLights(scene);
    ASSERT_TRUE(drawn("/World/rig/key"));
    recorder.reset();

    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateBBox));

    EXPECT_TRUE(drawn("/World/rig/key"));
    EXPECT_TRUE(drawn("/World/rig/key/fill"));
    EXPECT_TRUE(drawn("/World/rig/dome"));
    EXPECT_TRUE(drawn("/World/rig/subrig/spot"));
    EXPECT_FALSE(drawn("/World/rig/mesh"));
    EXPECT_EQ((PathSet { "/World/rig/mesh" }), recorder.hides);
    EXPECT_TRUE(filter->hasDrawnLights());
}

TEST_F(SceneDrawFilterTest, LightsStopInHidden)
{
    addLights(scene);

    filter->applyDelta(states.setState("/World/rig/subrig", eSceneDrawStateHidden));

    EXPECT_FALSE(drawn("/World/rig/subrig/spot"));
    EXPECT_TRUE(drawn("/World/rig/dome"));
    EXPECT_EQ((PathSet { "/World/rig/subrig/spot" }), recorder.hides);

    recorder.reset();
    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateHidden));

    EXPECT_FALSE(drawn("/World/rig/key"));
    EXPECT_FALSE(drawn("/World/rig/key/fill"));
    EXPECT_EQ(1u, recorder.hides.count("/World/rig/dome"));
    EXPECT_TRUE(filter->GetPrim(SdfPath("/World/rig/dome")).dataSource == nullptr);
}

TEST_F(SceneDrawFilterTest, LightsComeBackWithTheirTypeWhenAHiddenRowIsBoxedOrDrawn)
{
    addLights(scene);
    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateHidden));
    recorder.reset();

    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateBBox));

    EXPECT_EQ(1u, recorder.shows.count("/World/rig/dome"));
    EXPECT_EQ(HdPrimTypeTokens->domeLight, filter->GetPrim(SdfPath("/World/rig/dome")).primType);
    EXPECT_EQ(HdPrimTypeTokens->sphereLight, filter->GetPrim(SdfPath("/World/rig/subrig/spot")).primType);
    EXPECT_FALSE(drawn("/World/rig/mesh"));

    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateHidden));
    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateFull));

    EXPECT_TRUE(drawn("/World/rig/dome"));
    EXPECT_TRUE(drawn("/World/rig/mesh"));
}

TEST_F(SceneDrawFilterTest, AChildrenRowKeepsEveryLightAndOnlyBoxesItsGeometry)
{
    addLights(scene);

    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateChildren));

    EXPECT_TRUE(drawn("/World/rig/key"));
    EXPECT_TRUE(drawn("/World/rig/key/fill"));
    EXPECT_TRUE(drawn("/World/rig/dome"));
    EXPECT_TRUE(drawn("/World/rig/subrig/spot"));
    EXPECT_FALSE(drawn("/World/rig/mesh"));
}

TEST_F(SceneDrawFilterTest, ALightRigBesideADeepFullRowKeepsLighting)
{
    addLights(scene);

    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World/a/deep/m2", eSceneDrawStateFull));

    EXPECT_TRUE(drawn("/World/a/deep/m2"));
    EXPECT_FALSE(drawn("/World/b/m3"));
    EXPECT_TRUE(drawn("/World/rig/key"));
    EXPECT_TRUE(drawn("/World/rig/dome"));
    EXPECT_TRUE(drawn("/World/rig/subrig/spot"));
    EXPECT_TRUE(filter->hasDrawnLights());
}

TEST_F(SceneDrawFilterTest, ALightAddedBelowABoxedRowReachesHydraAndBelowAHiddenRowDoesNot)
{
    filter->applyDelta(states.setState("/World/b", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateHidden));
    recorder.reset();

    scene->AddPrims({ { SdfPath("/World/b/late"), HdPrimTypeTokens->rectLight, someData() } });
    EXPECT_TRUE(recorder.hides.empty());
    EXPECT_TRUE(drawn("/World/b/late"));

    scene->AddPrims({ { SdfPath("/World/a/late"), HdPrimTypeTokens->rectLight, someData() } });
    EXPECT_EQ((PathSet { "/World/a/late" }), recorder.hides);
    EXPECT_FALSE(drawn("/World/a/late"));
}

TEST_F(SceneDrawFilterTest, NoDrawnLightsUntilOneIsDrawn)
{
    EXPECT_FALSE(filter->hasDrawnLights());

    addLights(scene);
    EXPECT_TRUE(filter->hasDrawnLights());

    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateHidden));
    EXPECT_FALSE(filter->hasDrawnLights());

    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateBBox), 1);
    EXPECT_FALSE(filter->hasDrawnLights());
    while (filter->isRevealing()) {
        filter->stepReveal(1);
    }
    EXPECT_TRUE(filter->hasDrawnLights());
}

// A renderer's own light, such as the viewer's headlight, reaches the filter beside the stage's.
TEST_F(SceneDrawFilterTest, OnlyHiddenRowsHideLightsAndARendererLightCanBeLeftOut)
{
    addLights(scene);
    scene->AddPrims({ { SdfPath("/_Renderer/headlight"), HdPrimTypeTokens->simpleLight, someData() } });
    const SceneDrawFilter::LightPredicate ofTheStage = [](const SdfPath& light) {
        return light.HasPrefix(SdfPath("/World"));
    };
    EXPECT_FALSE(filter->hidesLights());

    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateBBox));
    EXPECT_FALSE(filter->hidesLights());
    EXPECT_TRUE(filter->hasDrawnLights(ofTheStage));

    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateHidden));
    EXPECT_TRUE(filter->hidesLights());
    EXPECT_FALSE(filter->hasDrawnLights(ofTheStage));
    EXPECT_TRUE(filter->hasDrawnLights()) << "the renderer's light is drawn";

    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateFull), 1);
    EXPECT_TRUE(filter->hidesLights()) << "the reveal has not reached the lights yet";
    while (filter->isRevealing()) {
        filter->stepReveal(1);
    }
    EXPECT_FALSE(filter->hidesLights());
    EXPECT_TRUE(filter->hasDrawnLights(ofTheStage));
}

TEST_F(SceneDrawFilterTest, RemovingTheLightsLeavesNoneDrawn)
{
    addLights(scene);
    ASSERT_TRUE(filter->hasDrawnLights());

    scene->RemovePrims({ { SdfPath("/World/rig") } });

    EXPECT_FALSE(filter->hasDrawnLights());
}

TEST_F(SceneDrawFilterTest, HiddenPrimsLeaveARealRenderIndex)
{
    addLights(scene);
    CountingRenderDelegate delegate;
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, HdDriverVector()));
    ASSERT_TRUE(index != nullptr);
    index->InsertSceneIndex(filter, SdfPath::AbsoluteRootPath());
    const SdfPath mesh("/World/a/m1");
    const SdfPath mat("/World/a/Looks/mat");
    const SdfPath light("/World/rig/key");
    const SdfPath dome("/World/rig/dome");
    ASSERT_TRUE(index->GetRprim(mesh));
    ASSERT_TRUE(index->GetSprim(HdPrimTypeTokens->rectLight, light));
    ASSERT_TRUE(index->GetSprim(HdPrimTypeTokens->domeLight, dome));

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateHidden));
    filter->applyDelta(states.setState("/World/rig", eSceneDrawStateHidden));

    EXPECT_FALSE(index->GetRprim(mesh));
    EXPECT_FALSE(index->GetRprim(SdfPath("/World/a/deep/m2")));
    EXPECT_FALSE(index->GetSprim(HdPrimTypeTokens->rectLight, light));
    EXPECT_FALSE(index->GetSprim(HdPrimTypeTokens->domeLight, dome));
    EXPECT_EQ(3, delegate.rprimsDestroyed);
    EXPECT_TRUE(index->GetRprim(SdfPath("/World/b/m3")));
    EXPECT_TRUE(index->GetSprim(HdPrimTypeTokens->material, mat));

    filter->applyDelta(states.clearState("/World/a"));
    filter->applyDelta(states.clearState("/World/rig"));

    EXPECT_TRUE(index->GetRprim(mesh));
    EXPECT_TRUE(index->GetSprim(HdPrimTypeTokens->rectLight, light));
}

// What makes boxing a row cheap in Storm: no rprim enters or leaves the render index, so the draw
// items are not gathered again and the draw batches are kept.
TEST_F(SceneDrawFilterTest, BoxingOnePrimChangesItsVisibilityAndNotTheRenderIndex)
{
    CountingRenderDelegate delegate;
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, HdDriverVector()));
    ASSERT_TRUE(index != nullptr);
    index->InsertSceneIndex(filter, SdfPath::AbsoluteRootPath());
    const SdfPath mesh("/World/a/deep/m2");
    ASSERT_TRUE(index->GetRprim(mesh));
    HdChangeTracker& tracker = index->GetChangeTracker();
    tracker.MarkRprimClean(mesh);
    const unsigned indexVersion = tracker.GetRprimIndexVersion();
    const unsigned visibilityChanges = tracker.GetVisibilityChangeCount();

    filter->applyDelta(states.setState("/World/a/deep", eSceneDrawStateBBox));

    EXPECT_EQ(indexVersion, tracker.GetRprimIndexVersion());
    EXPECT_NE(visibilityChanges, tracker.GetVisibilityChangeCount());
    EXPECT_NE(0u, tracker.GetRprimDirtyBits(mesh) & HdChangeTracker::DirtyVisibility);
    ASSERT_TRUE(index->GetRprim(mesh));
    HdSceneDelegate* sceneDelegate = index->GetSceneDelegateForRprim(mesh);
    ASSERT_TRUE(sceneDelegate != nullptr);
    EXPECT_FALSE(sceneDelegate->GetVisible(mesh));
    EXPECT_EQ(0, delegate.rprimsDestroyed);

    tracker.MarkRprimClean(mesh);
    filter->applyDelta(states.setState("/World/a/deep", eSceneDrawStateFull));

    EXPECT_EQ(indexVersion, tracker.GetRprimIndexVersion());
    EXPECT_NE(0u, tracker.GetRprimDirtyBits(mesh) & HdChangeTracker::DirtyVisibility);
    EXPECT_TRUE(sceneDelegate->GetVisible(mesh));

    filter->applyDelta(states.setState("/World/a/deep", eSceneDrawStateHidden));

    EXPECT_NE(indexVersion, tracker.GetRprimIndexVersion());
    EXPECT_FALSE(index->GetRprim(mesh));
    EXPECT_EQ(1, delegate.rprimsDestroyed);
}

TEST_F(SceneDrawFilterTest, ChangesToSeveralRowsAppliedTogetherAnnounceEachPrimOnce)
{
    const SceneDrawDelta world = states.setState("/World", eSceneDrawStateBBox);
    const SceneDrawDelta a = states.setState("/World/a", eSceneDrawStateFull);
    const SceneDrawDelta deep = states.setState("/World/a/deep", eSceneDrawStateHidden);

    filter->applyDeltas({ &world, &a, &deep });

    EXPECT_EQ((PathSet { "/World/a/deep/m2" }), recorder.leftHydra);
    EXPECT_EQ((PathSet { "/World/b/m3", "/World/b/ball", "/World/inst", "/World/c" }), recorder.invisible);
    EXPECT_TRUE(recorder.shows.empty());
    EXPECT_EQ(5u, filter->getHiddenSentCount());
    EXPECT_EQ(0u, filter->getShownSentCount());
    EXPECT_EQ((PathSet { "/World/a/m1" }), drawnGeometry());
}

TEST_F(SceneDrawFilterTest, AWideChangeAnnouncesOnlyThePrimsItChanges)
{
    filter->applyDelta(states.setState("/World/a/deep", eSceneDrawStateBBox));
    recorder.reset();
    const unsigned long long hiddenBefore = filter->getHiddenSentCount();

    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/b/m3", "/World/b/ball", "/World/inst", "/World/c" }), recorder.hides);
    EXPECT_EQ(5u, filter->getHiddenSentCount() - hiddenBefore);
    EXPECT_EQ(6u, filter->getHeldInvisibleCount());
}

TEST_F(SceneDrawFilterTest, AChangeDuringARevealKeepsWhatItAnnouncedThatTheNewRulesShow)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World", eSceneDrawStateFull), 2);
    filter->stepReveal(2);
    const PathSet announced = recorder.shows;
    ASSERT_EQ(2u, announced.size());
    recorder.reset();

    filter->applyDelta(states.setState("/World/a/deep", eSceneDrawStateBBox));

    EXPECT_FALSE(filter->isRevealing());
    PathSet expected = kAllGeometry;
    expected.erase("/World/a/deep/m2");
    EXPECT_EQ(expected, drawnGeometry());
    for (const std::string& path : recorder.hides) {
        EXPECT_EQ("/World/a/deep/m2", path);
    }
    for (const std::string& path : recorder.shows) {
        EXPECT_EQ(0u, announced.count(path)) << path << " was announced twice";
    }
}

TEST_F(SceneDrawFilterTest, APrimHiddenDuringARevealAndShownAgainIsRevealed)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World", eSceneDrawStateFull), 1);
    filter->applyDelta(states.setState("/World/b", eSceneDrawStateBBox), 1);
    filter->applyDelta(states.clearState("/World/b"), 1);

    while (filter->isRevealing()) {
        filter->stepReveal(1);
    }

    EXPECT_EQ(kAllGeometry, drawnGeometry());
    EXPECT_EQ(0u, filter->getHeldInvisibleCount());
}

namespace {
bool
hiddenByRules(const SceneDrawFilter::Rules& rules,
              const SdfPath& path,
              bool light)
{
    const SceneDrawFilter::Rules::const_iterator rule = SdfPathFindLongestPrefix(rules, path);
    if (rule == rules.end()) {
        return false;
    }
    if (light) {
        return rule->second == SceneDrawFilter::eRuleHidden;
    }
    switch (rule->second) {
    case SceneDrawFilter::eRuleShow:
        return false;
    case SceneDrawFilter::eRuleHide:
    case SceneDrawFilter::eRuleHidden:
        return true;
    case SceneDrawFilter::eRuleExpand:
        return rule->first != path;
    }

    return false;
}

// Every geometry prim and light of the input is drawn exactly when the filter's rules show it, and a
// Hidden row keeps it out of Hydra.
void
expectFilteredSceneFollowsTheRules(const HdSceneIndexBase& input,
                                   const SceneDrawFilter& filter)
{
    const std::set<TfToken> geometry = { HdPrimTypeTokens->mesh, HdPrimTypeTokens->sphere, HdPrimTypeTokens->instancer };
    SdfPathVector stack = input.GetChildPrimPaths(SdfPath::AbsoluteRootPath());
    std::size_t checked = 0;
    while (!stack.empty()) {
        const SdfPath path = stack.back();
        stack.pop_back();
        const SdfPathVector children = input.GetChildPrimPaths(path);
        stack.insert(stack.end(), children.begin(), children.end());
        const TfToken primType = input.GetPrim(path).primType;
        const bool light = HdPrimTypeIsLight(primType);
        if (!light && (geometry.count(primType) == 0)) {
            continue;
        }
        ++checked;
        const HdSceneIndexPrim prim = filter.GetPrim(path);
        const bool hidden = hiddenByRules(filter.getRules(), path, light);
        EXPECT_EQ(!hidden, !prim.primType.IsEmpty() && visible(prim)) << path;
        const SceneDrawFilter::Rules::const_iterator rule = SdfPathFindLongestPrefix(filter.getRules(), path);
        if (hidden && (rule != filter.getRules().end()) && (rule->second == SceneDrawFilter::eRuleHidden)) {
            EXPECT_TRUE(prim.primType.IsEmpty()) << path << " is under a Hidden row";
        }
    }
    EXPECT_LT(0u, checked);
}
} // anon namespace

TEST_F(SceneDrawFilterTest, TheFilteredSceneFollowsTheRulesThroughBatchedChangesAndInputChanges)
{
    addLights(scene);
    const SceneDrawDelta world = states.setState("/World", eSceneDrawStateBBox);
    const SceneDrawDelta a = states.setState("/World/a", eSceneDrawStateFull);
    const SceneDrawDelta deep = states.setState("/World/a/deep", eSceneDrawStateHidden);
    const SceneDrawDelta b = states.setState("/World/b", eSceneDrawStateChildren);
    const SceneDrawDelta rig = states.setState("/World/rig/subrig", eSceneDrawStateHidden);
    filter->applyDeltas({ &world, &a, &deep, &b, &rig });
    expectFilteredSceneFollowsTheRules(*scene, *filter);

    scene->AddPrims({ { SdfPath("/World/a/deep/late"), HdPrimTypeTokens->mesh, someData() },
                      { SdfPath("/World/b/late"), HdPrimTypeTokens->mesh, someData() },
                      { SdfPath("/World/d/e/late"), HdPrimTypeTokens->mesh, someData() },
                      { SdfPath("/World/rig/subrig/late"), HdPrimTypeTokens->sphereLight, someData() } });
    expectFilteredSceneFollowsTheRules(*scene, *filter);

    scene->RemovePrims({ { SdfPath("/World/a") } });
    const SceneDrawDelta deepCleared = states.clearState("/World/a/deep");
    const SceneDrawDelta worldFull = states.setState("/World", eSceneDrawStateFull);
    const SceneDrawDelta rigBoxed = states.setState("/World/rig", eSceneDrawStateBBox);
    filter->applyDeltas({ &deepCleared, &worldFull, &rigBoxed });
    expectFilteredSceneFollowsTheRules(*scene, *filter);

    scene->AddPrims({ { SdfPath("/World/a/m1"), HdPrimTypeTokens->mesh, someData() } });
    const SceneDrawDelta cHidden = states.setState("/World/c", eSceneDrawStateHidden);
    const SceneDrawDelta rigHidden = states.setState("/World/rig", eSceneDrawStateHidden);
    filter->applyDeltas({ &cHidden, &rigHidden }, 1);
    expectFilteredSceneFollowsTheRules(*scene, *filter);
    EXPECT_TRUE(drawn("/World/a/m1"));
}

namespace {
SceneDrawFilter::Rules
mixedRules(SceneDrawStates& states)
{
    SceneDrawFilter::Rules rules;
    SceneDrawFilter::applyToRules(states.setState("/World/a", eSceneDrawStateBBox), &rules);
    SceneDrawFilter::applyToRules(states.setState("/World/a/deep", eSceneDrawStateHidden), &rules);
    SceneDrawFilter::applyToRules(states.setState("/World/b", eSceneDrawStateChildren), &rules);
    SceneDrawFilter::applyToRules(states.setState("/World/c", eSceneDrawStateHidden), &rules);

    return rules;
}
} // anon namespace

TEST_F(SceneDrawFilterTest, ReplacingRulesWithTheCurrentRulesSendsNothing)
{
    const SceneDrawFilter::Rules rules = mixedRules(states);
    filter->seedRules(rules);

    filter->replaceRules(rules);

    EXPECT_TRUE(recorder.empty());
    EXPECT_EQ(0u, filter->getHiddenSentCount() + filter->getShownSentCount());
}

TEST_F(SceneDrawFilterTest, ReplacingRulesGoingToNoneAndBackSendsOnlyWhatChanged)
{
    const SceneDrawFilter::Rules rules = mixedRules(states);
    filter->seedRules(rules);
    const PathSet before = drawnGeometry();
    ASSERT_NE(kAllGeometry, before);

    filter->replaceRules(SceneDrawFilter::Rules());

    PathSet changed;
    for (const std::string& path : kAllGeometry) {
        if (before.count(path) == 0) {
            changed.insert(path);
        }
    }
    EXPECT_EQ(changed, recorder.shows);
    EXPECT_TRUE(recorder.hides.empty());
    EXPECT_TRUE(filter->getRules().empty());
    EXPECT_EQ(kAllGeometry, drawnGeometry());

    recorder.reset();
    filter->replaceRules(rules);

    EXPECT_EQ(changed, recorder.hides);
    EXPECT_TRUE(recorder.shows.empty());
    EXPECT_EQ(before, drawnGeometry());
    EXPECT_EQ((PathSet { "/World/c" }), recorder.leftHydra);
    PathSet boxed = changed;
    boxed.erase("/World/c");
    EXPECT_EQ(boxed, recorder.invisible);

    // A fresh filter's boxed prims were never in Hydra, so they are out of it, where this filter's
    // were shown a moment ago and are held invisible: both draw the same, and Hidden rows match.
    TfRefPtr<SceneDrawFilter> fresh = SceneDrawFilter::New(scene);
    fresh->seedRules(rules);
    for (const std::string& path : kAllGeometry) {
        const HdSceneIndexPrim a = filter->GetPrim(SdfPath(path));
        const HdSceneIndexPrim b = fresh->GetPrim(SdfPath(path));
        EXPECT_EQ(!b.primType.IsEmpty() && visible(b), !a.primType.IsEmpty() && visible(a)) << path;
        if (boxed.count(path) != 0) {
            EXPECT_FALSE(a.primType.IsEmpty()) << path;
            EXPECT_FALSE(visible(a)) << path;
            EXPECT_TRUE(b.primType.IsEmpty()) << path;
        } else {
            EXPECT_EQ(b.primType, a.primType) << path;
            EXPECT_EQ(visible(b), visible(a)) << path;
        }
    }
    EXPECT_EQ(fresh->getRules(), filter->getRules());
}

TEST_F(SceneDrawFilterTest, ReplacingRulesChangesOnlyTheRowsThatDiffer)
{
    SceneDrawFilter::Rules from = mixedRules(states);
    filter->seedRules(from);
    SceneDrawFilter::Rules to = from;
    to[SdfPath("/World/a")] = SceneDrawFilter::eRuleShow;

    filter->replaceRules(to);

    EXPECT_TRUE(recorder.hides.empty());
    EXPECT_TRUE(recorder.shows.count("/World/a/m1") == 1);
    for (const std::string& path : recorder.shows) {
        EXPECT_EQ(0u, path.find("/World/a/")) << path;
    }
    expectFilteredSceneFollowsTheRules(*scene, *filter);
}

TEST_F(SceneDrawFilterTest, ASubtreeRemovedAndAddedAgainFollowsTheRules)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    ASSERT_EQ(2u, filter->getHeldInvisibleCount());

    scene->RemovePrims({ { SdfPath("/World/a") } });

    EXPECT_EQ((PathSet { "/World/a" }), recorder.removed);
    EXPECT_EQ(0u, filter->getHeldInvisibleCount());
    recorder.reset();

    scene->AddPrims({ { SdfPath("/World/a"), TfToken("xform"), someData() }, { SdfPath("/World/a/m1"), HdPrimTypeTokens->mesh, someData() } });

    EXPECT_EQ((PathSet { "/World/a/m1" }), recorder.leftHydra);
    EXPECT_FALSE(drawn("/World/a/m1"));
    recorder.reset();

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull));

    EXPECT_EQ((PathSet { "/World/a/m1" }), recorder.shows);
    EXPECT_TRUE(drawn("/World/a/m1"));
}

TEST_F(SceneDrawFilterTest, RemovingPrimsDuringARevealLetsItFinish)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World", eSceneDrawStateFull), 1);
    filter->stepReveal(1);

    scene->RemovePrims({ { SdfPath("/World/b") }, { SdfPath("/World/a/deep") } });
    while (filter->isRevealing()) {
        filter->stepReveal(1);
    }

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/inst", "/World/c" }), drawnGeometry());
    EXPECT_EQ(0u, filter->getHeldInvisibleCount());
}

TEST_F(SceneDrawFilterTest, AFilterOnAnEmptySceneLearnsThePrimsAsTheyArrive)
{
    HdRetainedSceneIndexRefPtr empty = HdRetainedSceneIndex::New();
    TfRefPtr<SceneDrawFilter> late = SceneDrawFilter::New(empty);
    Recorder lateRecorder;
    late->AddObserver(HdSceneIndexObserverPtr(&lateRecorder));
    SceneDrawStates lateStates;
    late->applyDelta(lateStates.setState("/World/a", eSceneDrawStateBBox));
    EXPECT_TRUE(lateRecorder.empty());

    HdRetainedSceneIndex::AddedPrimEntries entries;
    SdfPathVector stack = scene->GetChildPrimPaths(SdfPath::AbsoluteRootPath());
    while (!stack.empty()) {
        const SdfPath path = stack.back();
        stack.pop_back();
        entries.push_back({ path, scene->GetPrim(path).primType, someData() });
        const SdfPathVector children = scene->GetChildPrimPaths(path);
        stack.insert(stack.end(), children.begin(), children.end());
    }
    empty->AddPrims(entries);

    EXPECT_EQ((PathSet { "/World/a/m1", "/World/a/deep/m2" }), lateRecorder.leftHydra);
    EXPECT_TRUE(late->GetPrim(SdfPath("/World/a/m1")).primType.IsEmpty());
    EXPECT_FALSE(late->GetPrim(SdfPath("/World/b/m3")).primType.IsEmpty());
    lateRecorder.reset();

    late->applyDelta(lateStates.setState("/World", eSceneDrawStateHidden));

    EXPECT_EQ((PathSet { "/World/b/m3", "/World/b/ball", "/World/inst", "/World/c" }), lateRecorder.leftHydra);
    lateRecorder.reset();

    late->applyDelta(lateStates.clearAllStates());

    EXPECT_EQ(kAllGeometry, lateRecorder.shows);
}

TEST_F(SceneDrawFilterTest, RegisteringTwiceIsHarmless)
{
    SceneDrawFilter::registerForAllRenderers();
    SceneDrawFilter::registerForAllRenderers();

    EXPECT_FALSE(get_pointer(SceneDrawFilter::find("no such engine")));
}

namespace {
std::unique_ptr<UsdImagingGLEngine>
makeCyclesEngine()
{
    UsdImagingGLEngine::Parameters parameters;
    parameters.rendererPluginId = TfToken(kRenderDelegateCyclesId);
    parameters.gpuEnabled = false;
    CyclesAovs::registerSceneIndices();
    std::unique_ptr<UsdImagingGLEngine> engine;
    CyclesAovs::withThreadBudget(2, [&]() { engine.reset(new UsdImagingGLEngine(parameters)); });

    return engine;
}
} // anon namespace

TEST_F(SceneDrawFilterTest, ACyclesEngineBuiltUnderAScopeHasAFilterFindCanLocate)
{
    if (!RenderDelegates::unavailableMessage(kRenderDelegateCyclesId).empty()) {
        return;
    }
    SceneDrawFilter::registerForAllRenderers();
    std::string id;
    std::unique_ptr<UsdImagingGLEngine> engine;
    {
        const SceneDrawFilter::EngineScope scope;
        engine = makeCyclesEngine();
        id = scope.getInstanceId();
    }
    ASSERT_TRUE(engine != nullptr);
    ASSERT_EQ(TfToken(kRenderDelegateCyclesId), engine->GetCurrentRendererId());

    EXPECT_FALSE(id.empty());
    EXPECT_TRUE(get_pointer(SceneDrawFilter::find(id)) != nullptr);
}

namespace {
std::set<std::string>
liveFilterIds()
{
    const std::vector<std::string> ids = SceneDrawFilter::getInstanceIds();

    return std::set<std::string>(ids.begin(), ids.end());
}

bool
chainHasDrawFilter(const HdSceneIndexBaseRefPtr& scene)
{
    HdSceneIndexBase* const base = get_pointer(scene);
    if (dynamic_cast<SceneDrawFilter*>(base)) {
        return true;
    }
    const HdFilteringSceneIndexBase* const filtering = dynamic_cast<const HdFilteringSceneIndexBase*>(base);
    if (!filtering) {
        return false;
    }
    for (const HdSceneIndexBaseRefPtr& input : filtering->GetInputScenes()) {
        if (chainHasDrawFilter(input)) {
            return true;
        }
    }

    return false;
}

const char* const kChainRenderer = "NatronDrawFilterTestRenderer";
} // anon namespace

TEST_F(SceneDrawFilterTest, ACyclesEngineBuiltOnAnotherThreadWhileAScopeLivesHasNoFilter)
{
    if (!RenderDelegates::unavailableMessage(kRenderDelegateCyclesId).empty()) {
        return;
    }
    SceneDrawFilter::registerForAllRenderers();
    const std::set<std::string> before = liveFilterIds();
    std::unique_ptr<UsdImagingGLEngine> unscoped;
    std::unique_ptr<UsdImagingGLEngine> scoped;
    std::string id;
    {
        const SceneDrawFilter::EngineScope scope;
        std::thread other([&]() { unscoped = makeCyclesEngine(); });
        other.join();
        ASSERT_TRUE(unscoped != nullptr);
        ASSERT_EQ(TfToken(kRenderDelegateCyclesId), unscoped->GetCurrentRendererId());

        EXPECT_EQ(before, liveFilterIds());
        EXPECT_TRUE(scope.getInstanceId().empty());

        scoped = makeCyclesEngine();
        id = scope.getInstanceId();
    }
    ASSERT_TRUE(scoped != nullptr);

    std::set<std::string> expected = before;
    expected.insert(id);
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(expected, liveFilterIds());
}

TEST_F(SceneDrawFilterTest, ConcurrentChainsGetAFilterOnlyWhereAScopeAskedForIt)
{
    SceneDrawFilter::registerForAllRenderers();
    HdSceneIndexPluginRegistry& registry = HdSceneIndexPluginRegistry::GetInstance();
    EXPECT_FALSE(chainHasDrawFilter(registry.AppendSceneIndicesForRenderer(kChainRenderer, HdRetainedSceneIndex::New(), "warm-up")));

    const int kThreads = 8;
    for (int round = 0; round < 20; ++round) {
        std::vector<HdSceneIndexBaseRefPtr> chains(kThreads);
        std::vector<std::string> scopeIds(kThreads);
        std::latch start(kThreads);
        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i]() {
                const std::string instanceId = "chain-" + std::to_string(round) + "-" + std::to_string(i);
                start.arrive_and_wait();
                if (i % 2 == 0) {
                    const SceneDrawFilter::EngineScope scope;
                    chains[i] = registry.AppendSceneIndicesForRenderer(kChainRenderer, HdRetainedSceneIndex::New(), instanceId);
                    scopeIds[i] = scope.getInstanceId();
                } else {
                    chains[i] = registry.AppendSceneIndicesForRenderer(kChainRenderer, HdRetainedSceneIndex::New(), instanceId);
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }

        for (int i = 0; i < kThreads; ++i) {
            const std::string instanceId = "chain-" + std::to_string(round) + "-" + std::to_string(i);
            if (i % 2 == 0) {
                EXPECT_TRUE(chainHasDrawFilter(chains[i])) << instanceId;
                EXPECT_EQ(instanceId, scopeIds[i]);
                EXPECT_TRUE(get_pointer(SceneDrawFilter::find(instanceId)) != nullptr) << instanceId;
            } else {
                EXPECT_FALSE(chainHasDrawFilter(chains[i])) << instanceId;
                EXPECT_FALSE(get_pointer(SceneDrawFilter::find(instanceId))) << instanceId;
            }
        }
    }
}

TEST_F(SceneDrawFilterTest, ABoxedRowMasksTheNativeInstancesBelowItAndKeepsTheirInstancer)
{
    addInstances(scene);
    recorder.reset();

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));

    EXPECT_EQ((std::vector<bool> { false, false, true }), maskOf(*filter, kNativeInstancer));
    EXPECT_EQ(2u, filter->getMaskedInstanceCount());
    EXPECT_EQ(1u, recorder.dirtied.count(kNativeInstancer));
    EXPECT_EQ(HdPrimTypeTokens->instancer, filter->GetPrim(SdfPath(kNativeInstancer)).primType);
    EXPECT_TRUE(drawn("/UsdNiPropagatedPrototypes/Bindings/__Prototype_1/UsdNiInstancer/UsdNiPrototype/m"));
    recorder.reset();

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull));

    EXPECT_TRUE(maskOf(*filter, kNativeInstancer).empty());
    EXPECT_EQ(0u, filter->getMaskedInstanceCount());
    EXPECT_EQ(1u, recorder.dirtied.count(kNativeInstancer));
}

TEST_F(SceneDrawFilterTest, AHiddenRowMasksItsInstancesAndARowOnOneInstanceMasksOnlyIt)
{
    addInstances(scene);

    filter->applyDelta(states.setState("/World/b", eSceneDrawStateHidden));

    EXPECT_EQ((std::vector<bool> { true, true, false }), maskOf(*filter, kNativeInstancer));
    EXPECT_EQ(HdPrimTypeTokens->instancer, filter->GetPrim(SdfPath(kNativeInstancer)).primType);

    filter->applyDelta(states.setState("/World/a/i2", eSceneDrawStateChildren));

    EXPECT_EQ((std::vector<bool> { true, false, false }), maskOf(*filter, kNativeInstancer));

    filter->applyDelta(states.clearAllStates());

    EXPECT_TRUE(maskOf(*filter, kNativeInstancer).empty());
}

TEST_F(SceneDrawFilterTest, RowsOutsideTheInstancesLeaveTheirMaskAlone)
{
    addInstances(scene);
    recorder.reset();

    filter->applyDelta(states.setState("/World/c", eSceneDrawStateBBox));

    EXPECT_TRUE(maskOf(*filter, kNativeInstancer).empty());
    EXPECT_EQ(0u, recorder.dirtied.count(kNativeInstancer));
}

TEST_F(SceneDrawFilterTest, TheMaskKeepsWhatTheInputMasks)
{
    addInstances(scene, { true, false, true });

    filter->applyDelta(states.setState("/World/b", eSceneDrawStateBBox));

    EXPECT_EQ((std::vector<bool> { true, false, false }), maskOf(*filter, kNativeInstancer));
}

TEST_F(SceneDrawFilterTest, AnInstancerAddedOrSeededUnderRulesStartsMasked)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    addInstances(scene);

    EXPECT_EQ((std::vector<bool> { false, false, true }), maskOf(*filter, kNativeInstancer));

    SceneDrawFilter::Rules rules;
    SceneDrawFilter::applyToRules(states.setState("/World/b", eSceneDrawStateBBox), &rules);
    TfRefPtr<SceneDrawFilter> fresh = SceneDrawFilter::New(scene);
    fresh->seedRules(rules);

    EXPECT_EQ((std::vector<bool> { true, true, false }), maskOf(*fresh, kNativeInstancer));
    EXPECT_EQ(1u, fresh->getMaskedInstanceCount());
}

TEST_F(SceneDrawFilterTest, ANewInstanceListIsReadWhenTheInputAddsItAgain)
{
    addInstances(scene);
    filter->applyDelta(states.setState("/World/b", eSceneDrawStateBBox));

    scene->AddPrims({ { SdfPath(kNativeInstancer), HdPrimTypeTokens->instancer, instancerData({ "/World/b/i3", "/World/a/i1" }) } });

    EXPECT_EQ((std::vector<bool> { false, true }), maskOf(*filter, kNativeInstancer));
    EXPECT_EQ(1u, filter->getMaskedInstanceCount());

    scene->RemovePrims({ SdfPath("/UsdNiPropagatedPrototypes") });

    EXPECT_EQ(0u, filter->getMaskedInstanceCount());
}

namespace {
// The prims announced with their type, in the order they went.
class ShowOrder
    : public HdSceneIndexObserver {
public:
    void PrimsAdded(const HdSceneIndexBase& /*sender*/,
                    const AddedPrimEntries& entries) override
    {
        for (const AddedPrimEntry& entry : entries) {
            if (!entry.primType.IsEmpty()) {
                shown.push_back(entry.primPath.GetString());
            }
        }
    }

    void PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                      const RemovedPrimEntries& /*entries*/) override
    {
    }

    void PrimsDirtied(const HdSceneIndexBase& sender,
                      const DirtiedPrimEntries& entries) override
    {
        static const HdDataSourceLocatorSet visibilityOnly { HdVisibilitySchema::GetDefaultLocator() };
        for (const DirtiedPrimEntry& entry : entries) {
            if ((entry.dirtyLocators == visibilityOnly) && visible(sender.GetPrim(entry.primPath))) {
                shown.push_back(entry.primPath.GetString());
            }
        }
    }

    void PrimsRenamed(const HdSceneIndexBase& /*sender*/,
                      const RenamedPrimEntries& /*entries*/) override
    {
    }

    std::vector<std::string> shown;
};

PathSet
toSet(const std::vector<std::string>& paths)
{
    return PathSet(paths.begin(), paths.end());
}

void
revealAll(SceneDrawFilter& filter,
          std::size_t batch)
{
    while (filter.isRevealing()) {
        filter.stepReveal(batch);
    }
}

// What a payload landing at /World/a brings in, as usdImaging resyncs it.
void
addPayload(const HdRetainedSceneIndexRefPtr& scene)
{
    scene->AddPrims({ { SdfPath("/World/a/payload"), TfToken("xform"), someData() },
                      { SdfPath("/World/a/payload/p1"), HdPrimTypeTokens->mesh, someData() },
                      { SdfPath("/World/a/payload/p2"), HdPrimTypeTokens->mesh, someData() },
                      { SdfPath("/World/a/payload/lamp"), HdPrimTypeTokens->sphereLight, someData() } });
}

void
appendInputOrder(const HdSceneIndexBase& scene,
                 const SdfPath& path,
                 std::vector<std::string>* order)
{
    if (kAllGeometry.count(path.GetString()) != 0) {
        order->push_back(path.GetString());
    }
    for (const SdfPath& child : scene.GetChildPrimPaths(path)) {
        appendInputOrder(scene, child, order);
    }
}

// The geometry below root, depth first in the order the input lists children. A retained scene
// index lists them newest first, not in the order they were added.
std::vector<std::string>
inputOrder(const HdSceneIndexBase& scene,
           const char* root)
{
    std::vector<std::string> order;
    appendInputOrder(scene, SdfPath(root), &order);

    return order;
}
} // anon namespace

TEST_F(SceneDrawFilterTest, ARevealGoesDepthFirstInTheInputsOrder)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    ShowOrder order;
    filter->AddObserver(HdSceneIndexObserverPtr(&order));

    filter->applyDelta(states.setState("/World", eSceneDrawStateFull), 1);
    revealAll(*filter, 1);

    const std::vector<std::string> expected = inputOrder(*scene, "/World");
    EXPECT_EQ(kAllGeometry.size(), expected.size());
    EXPECT_EQ(expected, order.shown);
    filter->RemoveObserver(HdSceneIndexObserverPtr(&order));
}

TEST_F(SceneDrawFilterTest, ARevealOfSeveralRowsFinishesOneBeforeTheNextAndTheLatestChangeComesFirst)
{
    const SceneDrawDelta boxA = states.setState("/World/a", eSceneDrawStateBBox);
    const SceneDrawDelta boxB = states.setState("/World/b", eSceneDrawStateBBox);
    filter->applyDeltas({ &boxA, &boxB });
    ShowOrder order;
    filter->AddObserver(HdSceneIndexObserverPtr(&order));

    const SceneDrawDelta fullA = states.setState("/World/a", eSceneDrawStateFull);
    const SceneDrawDelta fullB = states.setState("/World/b", eSceneDrawStateFull);
    filter->applyDeltas({ &fullA, &fullB }, 1);
    revealAll(*filter, 1);

    const std::vector<std::string> rowA = inputOrder(*scene, "/World/a");
    const std::vector<std::string> rowB = inputOrder(*scene, "/World/b");
    ASSERT_EQ(2u, rowA.size());
    ASSERT_EQ(2u, rowB.size());
    std::vector<std::string> expected = rowA;
    expected.insert(expected.end(), rowB.begin(), rowB.end());
    EXPECT_EQ(expected, order.shown);
    order.shown.clear();

    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World/b", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World/b", eSceneDrawStateFull), 1);
    EXPECT_EQ(1u, filter->stepReveal(1));
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull), 1);
    revealAll(*filter, 1);

    expected = { rowB.front() };
    expected.insert(expected.end(), rowA.begin(), rowA.end());
    expected.insert(expected.end(), rowB.begin() + 1, rowB.end());
    EXPECT_EQ(expected, order.shown);
    filter->RemoveObserver(HdSceneIndexObserverPtr(&order));
}

TEST_F(SceneDrawFilterTest, BoxingTheRowOfARevealStopsItWithinOneStep)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/World", eSceneDrawStateFull), 1);
    filter->stepReveal(1);
    const PathSet announced = recorder.shows;
    ASSERT_EQ(1u, announced.size());
    recorder.reset();

    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox), 1);

    EXPECT_EQ(announced, recorder.hides);
    EXPECT_EQ(0u, filter->stepReveal(1));
    EXPECT_FALSE(filter->isRevealing());
    EXPECT_TRUE(recorder.shows.empty());
    EXPECT_TRUE(drawnGeometry().empty());
}

TEST_F(SceneDrawFilterTest, BoxingARevealFromHiddenStillBringsBackItsLightsAndNothingElse)
{
    addLights(scene);
    filter->applyDelta(states.setState("/World", eSceneDrawStateHidden));
    filter->applyDelta(states.setState("/World", eSceneDrawStateFull), 1);
    filter->stepReveal(1);
    recorder.reset();

    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox), 1);
    std::size_t steps = 0;
    while (filter->isRevealing()) {
        filter->stepReveal(1);
        ++steps;
    }

    // One step a light, and one to find nothing is left.
    EXPECT_LE(steps, 5u);
    EXPECT_TRUE(drawnGeometry().empty());
    EXPECT_FALSE(drawn("/World/rig/mesh"));
    for (const char* light : { "/World/rig/key", "/World/rig/key/fill", "/World/rig/dome", "/World/rig/subrig/spot" }) {
        EXPECT_TRUE(drawn(light)) << light;
    }
    for (const std::string& path : recorder.shows) {
        EXPECT_EQ(0u, kAllGeometry.count(path)) << path;
    }
}

TEST_F(SceneDrawFilterTest, OverlappingRevealsAnnounceEachPrimOnceAndFinish)
{
    filter->applyDelta(states.setState("/World", eSceneDrawStateBBox));
    ShowOrder order;
    filter->AddObserver(HdSceneIndexObserverPtr(&order));

    // A row below a reveal under way, then the reveal's own row again, then a row above both.
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateFull), 1);
    filter->stepReveal(1);
    filter->applyDelta(states.setState("/World/a/deep", eSceneDrawStateFull), 1);
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateLoadAll), 1);
    filter->stepReveal(1);
    filter->applyDelta(states.setState("/World", eSceneDrawStateFull), 1);
    revealAll(*filter, 1);

    EXPECT_EQ(kAllGeometry, drawnGeometry());
    EXPECT_EQ(kAllGeometry, toSet(order.shown));
    EXPECT_EQ(kAllGeometry.size(), order.shown.size());
    filter->RemoveObserver(HdSceneIndexObserverPtr(&order));
}

TEST_F(SceneDrawFilterTest, ARowChangedAgainDuringItsRevealStillRevealsWhatItHadLeft)
{
    SceneDrawDelta box;
    box.boxed = { "/World/b" };
    SceneDrawDelta show;
    show.drawn = { "/World/b" };
    filter->applyDelta(box);
    filter->applyDelta(show, 1);
    ASSERT_EQ(1u, filter->stepReveal(1));

    // The row's rule is what it was, so only the frontier left there knows what is still to come.
    filter->applyDelta(show, 1);
    revealAll(*filter, 1);

    EXPECT_EQ(kAllGeometry, drawnGeometry());
}

TEST_F(SceneDrawFilterTest, WhatALoadAllBringsInJoinsAReveal)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateLoadAll), 1);
    recorder.reset();

    addPayload(scene);

    EXPECT_TRUE(filter->isRevealing());
    EXPECT_EQ((PathSet { "/World/a/payload/p1", "/World/a/payload/p2" }), recorder.leftHydra);
    EXPECT_EQ((PathSet { "/World/a/payload", "/World/a/payload/lamp" }), recorder.shows);
    EXPECT_FALSE(drawn("/World/a/payload/p1"));

    scene->AddPrims({ { SdfPath("/World/c2"), HdPrimTypeTokens->mesh, someData() } });
    EXPECT_TRUE(drawn("/World/c2"));

    EXPECT_EQ(1u, filter->stepReveal(1));
    EXPECT_TRUE(drawn("/World/a/payload/p1"));
    EXPECT_FALSE(drawn("/World/a/payload/p2"));
    revealAll(*filter, 1);
    EXPECT_TRUE(drawn("/World/a/payload/p2"));
    EXPECT_EQ(kAllGeometry, drawnGeometry());
}

TEST_F(SceneDrawFilterTest, APrimResyncedWithinARenderComesBackAsHydraHeldIt)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateLoadAll), 1);
    addPayload(scene);
    revealAll(*filter, 1);
    filter->endRender();
    recorder.reset();

    scene->RemovePrims({ SdfPath("/World/a/payload") });
    addPayload(scene);

    EXPECT_FALSE(filter->isRevealing());
    EXPECT_TRUE(drawn("/World/a/payload/p1"));
    EXPECT_TRUE(drawn("/World/a/payload/p2"));
    EXPECT_TRUE(recorder.leftHydra.empty());
    filter->endRender();

    scene->RemovePrims({ SdfPath("/World/a/payload") });
    filter->endRender();
    addPayload(scene);

    EXPECT_TRUE(filter->isRevealing());
    EXPECT_FALSE(drawn("/World/a/payload/p1"));
}

TEST_F(SceneDrawFilterTest, AChangeShownAtOnceLetsLoadsArriveAtOnce)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateLoadAll), 1);
    filter->applyDelta(states.setState("/World/c", eSceneDrawStateBBox));

    addPayload(scene);

    EXPECT_FALSE(filter->isRevealing());
    EXPECT_TRUE(drawn("/World/a/payload/p1"));
    EXPECT_TRUE(drawn("/World/a/payload/p2"));
}

TEST_F(SceneDrawFilterTest, BoxingTheRowOfALoadAllLeavesWhatArrivesBoxed)
{
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateLoadAll), 1);
    filter->applyDelta(states.setState("/World/a", eSceneDrawStateBBox), 1);

    addPayload(scene);
    revealAll(*filter, 1);

    EXPECT_FALSE(drawn("/World/a/payload/p1"));
    EXPECT_FALSE(drawn("/World/a/payload/p2"));
    EXPECT_TRUE(drawn("/World/a/payload/lamp"));
}

TEST_F(SceneDrawFilterTest, AStepStopsAtItsDeadline)
{
    const std::size_t wide = 3000;
    HdRetainedSceneIndex::AddedPrimEntries entries { { SdfPath("/Wide"), TfToken("xform"), someData() } };
    for (std::size_t i = 0; i < wide; ++i) {
        entries.push_back({ SdfPath("/Wide/m" + std::to_string(i)), HdPrimTypeTokens->mesh, someData() });
    }
    scene->AddPrims(entries);
    filter->applyDelta(states.setState("/Wide", eSceneDrawStateBBox));
    filter->applyDelta(states.setState("/Wide", eSceneDrawStateFull), 1);

    const std::size_t shown = filter->stepReveal(wide, SceneDrawFilter::Clock::now() - std::chrono::seconds(1));

    EXPECT_TRUE(filter->wasStepCut());
    EXPECT_LT(shown, wide);
    EXPECT_TRUE(filter->isRevealing());

    EXPECT_EQ(wide - shown, filter->stepReveal(wide));
    EXPECT_FALSE(filter->wasStepCut());
    EXPECT_FALSE(filter->isRevealing());
}

TEST_F(SceneDrawFilterTest, ADeepSceneIsIndexedAndRevealedWithoutRecursing)
{
    std::string path = "/Deep";
    for (int i = 0; i < 3000; ++i) {
        path += "/x";
    }
    const std::string leaf = path + "/m";
    scene->AddPrims({ { SdfPath(leaf), HdPrimTypeTokens->mesh, someData() } });
    filter->applyDelta(states.setState("/Deep", eSceneDrawStateBBox));
    ASSERT_FALSE(drawn(leaf.c_str()));

    filter->applyDelta(states.setState("/Deep", eSceneDrawStateFull), 1);
    revealAll(*filter, 1);

    EXPECT_TRUE(drawn(leaf.c_str()));
    scene->RemovePrims({ SdfPath("/Deep") });
    EXPECT_FALSE(filter->isRevealing());
}

namespace {
// Feeds the pacer frames costing fixedMs plus perPrimMs a prim, and returns the batches it chose.
std::vector<std::size_t>
pace(SceneRevealPacer& pacer,
     double fixedMs,
     double perPrimMs,
     int frames)
{
    std::vector<std::size_t> batches;
    for (int i = 0; i < frames; ++i) {
        const std::size_t batch = pacer.nextBatch();
        batches.push_back(batch);
        pacer.observe(batch, fixedMs + perPrimMs * double(batch));
    }
    batches.push_back(pacer.nextBatch());

    return batches;
}
} // anon namespace

TEST(SceneRevealPacerTest, BatchesFillTheTargetFrame)
{
    SceneRevealPacer pacer(33., 1024);
    pace(pacer, 0., 0.01, 30);

    EXPECT_NEAR(3300., double(pacer.nextBatch()), 33.);
}

TEST(SceneRevealPacerTest, TheFixedPartOfAFrameIsLeftOutOfTheBatch)
{
    SceneRevealPacer pacer(33., 1024);
    pace(pacer, 10., 0.001, 30);

    EXPECT_NEAR(23000., double(pacer.nextBatch()), 230.);
    EXPECT_NEAR(10., pacer.getFixedMs(), 0.1);
}

TEST(SceneRevealPacerTest, AFasterMachineTakesLargerBatches)
{
    SceneRevealPacer modest(33., 1024);
    SceneRevealPacer manyCore(33., 1024);
    pace(modest, 5., 0.02, 30);
    pace(manyCore, 5., 0.002, 30);

    EXPECT_GT(manyCore.nextBatch(), 5 * modest.nextBatch());
}

TEST(SceneRevealPacerTest, AFrameTheFixedPartFillsSpendsAsMuchAgainOnTheReveal)
{
    SceneRevealPacer pacer(33., 1024);
    const std::vector<std::size_t> batches = pace(pacer, 600., 0.0006, 30);

    for (std::size_t i = 1; i < batches.size(); ++i) {
        EXPECT_LE(batches[i], 2 * batches[i - 1]);
    }
    EXPECT_NEAR(1e6, double(pacer.nextBatch()), 1e4);
}

TEST(SceneRevealPacerTest, ASlowFrameShrinksTheBatchByAQuarterAtMost)
{
    SceneRevealPacer pacer(33., 1024);
    pacer.observe(1024, 10000.);

    EXPECT_EQ(256u, pacer.nextBatch());
    pacer.observe(0, 50.);
    EXPECT_EQ(256u, pacer.nextBatch());
    for (int i = 0; i < 20; ++i) {
        pacer.observe(pacer.nextBatch(), 10000.);
    }
    EXPECT_GE(pacer.nextBatch(), 1u);
}
