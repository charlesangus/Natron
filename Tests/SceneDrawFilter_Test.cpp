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

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Gui/SceneDrawFilter.h"

#ifdef emit
#define NATRON_SCENEDRAWFILTER_TEST_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/unitTestNullRenderDelegate.h>
#include <pxr/imaging/hd/visibilitySchema.h>
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

TEST_F(SceneDrawFilterTest, RegisteringTwiceIsHarmless)
{
    SceneDrawFilter::registerForAllRenderers();
    SceneDrawFilter::registerForAllRenderers();

    EXPECT_TRUE(SceneDrawFilter::takeAppendedInstanceId().empty());
    EXPECT_FALSE(get_pointer(SceneDrawFilter::find("no such engine")));
}
