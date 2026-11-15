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

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "SceneTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"
#include "Tests/Scene/SceneTestAuthoring.h"

NATRON_NAMESPACE_USING

namespace {
// /World/Block_<b>/B_<n> on disk, one payload file per building.
class PoolCity {
public:
    PoolCity(int blocks,
             int buildingsPerBlock)
    {
        static std::atomic<int> counter(0);

        _dir = std::filesystem::temp_directory_path() / ("scene_pool_city_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(_dir / "buildings");
        _dir = std::filesystem::canonical(_dir);
        std::ofstream root((_dir / "root.usda").string());
        root << "#usda 1.0\n(\n    defaultPrim = \"World\"\n)\ndef Xform \"World\"\n{\n";
        for (int b = 0; b < blocks; ++b) {
            root << "    def Xform \"Block_" << b << "\"\n    {\n";
            for (int n = 0; n < buildingsPerBlock; ++n) {
                const std::string name = "b" + std::to_string(b) + "_" + std::to_string(n) + ".usda";
                std::ofstream building((_dir / "buildings" / name).string());
                building << "#usda 1.0\n(\n    defaultPrim = \"Building\"\n)\ndef Xform \"Building\"\n{\n    def Cube \"Geo\"\n    {\n    }\n}\n";
                root << "        def Xform \"B_" << n << "\" (\n            prepend payload = @./buildings/" << name << "@\n        )\n        {\n        }\n";
            }
            root << "    }\n";
        }
        root << "}\n";
    }

    ~PoolCity()
    {
        std::error_code ignored;
        std::filesystem::remove_all(_dir, ignored);
    }

    ScenePayload payload() const
    {
        const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen((_dir / "root.usda").string());

        EXPECT_TRUE(bool(layer));

        return SceneOps::appendLayer(ScenePayload(), layer, 1);
    }

    // The buildings' files open anywhere in the process, by file name.
    std::vector<std::string> loadedBuildings() const
    {
        const std::string prefix = (_dir / "buildings").string();
        std::vector<std::string> names;

        for (const PXR_NS::SdfLayerHandle& layer : PXR_NS::SdfLayer::GetLoadedLayers()) {
            const std::string path = layer->GetRealPath();
            if (path.compare(0, prefix.size(), prefix) == 0) {
                names.push_back(std::filesystem::path(path).filename().string());
            }
        }
        std::sort(names.begin(), names.end());

        return names;
    }

private:
    std::filesystem::path _dir;
};

// Loads nothing but the subtrees of paths.
SceneLoadSetPtr
loading(const std::vector<std::string>& paths)
{
    std::vector<SceneLoadSet::Entry> entries;
    SceneLoadSet::Entry none;

    none.path = "/";
    none.rule = SceneLoadSet::eRuleNone;
    entries.push_back(none);
    for (const std::string& path : paths) {
        SceneLoadSet::Entry all;
        all.path = path;
        all.rule = SceneLoadSet::eRuleAll;
        entries.push_back(all);
    }

    return std::make_shared<const SceneLoadSet>(entries);
}
} // anonymous namespace

TEST(SceneQueryPoolLoadSets, ABorrowedStageLoadsOnlyTheSetsPayloads)
{
    PoolCity city(2, 2);
    SceneQueryPool pool(2);
    std::string error;

    {
        SceneQueryPool::Lease lease = pool.borrow(city.payload(), loading({ "/World/Block_1/B_0" }), &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(city.loadedBuildings(), std::vector<std::string>({ "b1_0.usda" }));
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_1/B_0")).IsLoaded());
        const PXR_NS::UsdPrim unloaded = stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_0/B_0"));
        ASSERT_TRUE(bool(unloaded));
        EXPECT_FALSE(unloaded.IsLoaded());
    }
    pool.releaseIdle();
    EXPECT_TRUE(city.loadedBuildings().empty());
}

TEST(SceneQueryPoolLoadSets, ANullSetLoadsEveryPayload)
{
    PoolCity city(1, 2);
    SceneQueryPool pool(2);
    std::string error;

    SceneQueryPool::Lease lease = pool.borrow(city.payload(), SceneLoadSetPtr(), &error);
    ASSERT_TRUE(lease.isValid()) << error;
    EXPECT_EQ(city.loadedBuildings(), std::vector<std::string>({ "b0_0.usda", "b0_1.usda" }));
}

TEST(SceneQueryPoolLoadSets, TheSameSetAgainChangesNoLoads)
{
    PoolCity city(1, 3);
    SceneQueryPool pool(2);
    std::string error;
    const SceneLoadSetPtr set = loading({ "/World/Block_0/B_1" });
    PXR_NS::UsdStage* first = nullptr;
    U64 loadEdits = 0;
    {
        SceneQueryPool::Lease lease = pool.borrow(city.payload(), set, &error);
        ASSERT_TRUE(lease.isValid()) << error;
        first = lease.getStage().operator->();
        loadEdits = lease.getLoadEditCount();
    }
    {
        // An equal set, not the same object.
        SceneQueryPool::Lease lease = pool.borrow(city.payload(), loading({ "/World/Block_0/B_1" }), &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(first, lease.getStage().operator->());
        EXPECT_EQ(loadEdits, lease.getLoadEditCount());
    }
    {
        // Another set changes the idle stage's loads rather than opening a stage.
        SceneQueryPool::Lease lease = pool.borrow(city.payload(), loading({ "/World/Block_0/B_2" }), &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(first, lease.getStage().operator->());
        EXPECT_GT(lease.getLoadEditCount(), loadEdits);
        EXPECT_FALSE(lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_0/B_1")).IsLoaded());
        EXPECT_TRUE(lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_0/B_2")).IsLoaded());
    }
}

TEST(SceneQueryPoolLoadSets, WithPathsLoadsOnlyTheChainOfAKnownPath)
{
    CarCity city(2, 3);
    SceneQueryPool pool(2);
    std::string error;
    const std::string car = carPath(0, 1);
    const SceneLoadSetPtr none = std::make_shared<const SceneLoadSet>(SceneLoadSet::none());
    const SceneLoadSetPtr set = SceneLoadSet::withPaths(none, std::vector<std::string>(1, car));

    ASSERT_TRUE(bool(set));
    EXPECT_EQ(*set, *SceneLoadSet::withPaths(none, std::vector<std::string>(1, car)));
    EXPECT_FALSE(SceneLoadSet::withPaths(SceneLoadSetPtr(), std::vector<std::string>(1, car)));

    PXR_NS::TfToken type;
    std::vector<PXR_NS::TfToken> properties;
    {
        SceneQueryPool::Lease lease = pool.borrow(city.payload(), set, &error);
        ASSERT_TRUE(lease.isValid()) << error;
        EXPECT_EQ(city.openedBuildings(), 1);
        const PXR_NS::UsdPrim prim = lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath(car));
        ASSERT_TRUE(bool(prim));
        type = prim.GetTypeName();
        properties = prim.GetPropertyNames();
    }
    pool.releaseIdle();

    SceneQueryPool::Lease full = pool.borrow(city.payload(), SceneLoadSetPtr(), &error);
    ASSERT_TRUE(full.isValid()) << error;
    EXPECT_EQ(city.openedBuildings(), city.getNumBuildings());
    const PXR_NS::UsdPrim reference = full.getStage()->GetPrimAtPath(PXR_NS::SdfPath(car));
    ASSERT_TRUE(bool(reference));
    EXPECT_EQ(type, reference.GetTypeName());
    EXPECT_EQ(properties, reference.GetPropertyNames());
}

namespace {
SceneLoadSetPtr
withRules(const std::vector<SceneLoadSet::Entry>& entries)
{
    return std::make_shared<const SceneLoadSet>(entries);
}
} // anonymous namespace

TEST(SceneLoadSetUnite, IncludesExactlyWhatEitherSetDoesOnEveryPrim)
{
    CarCity city(2, 3);
    SceneQueryPool pool(1);
    std::string error;
    const SceneLoadSetPtr none = std::make_shared<const SceneLoadSet>(SceneLoadSet::none());
    const std::string building = "/World/Block_0/B_1";
    const std::vector<std::pair<SceneLoadSetPtr, SceneLoadSetPtr>> pairs = {
        { loadingOnly({ building }), SceneLoadSet::withPaths(none, { carPath(1, 2) }) },
        { SceneLoadSet::withPaths(none, { carPath(0, 0) }), loadingOnly({ "/World/Block_0" }) },
        { none, loadingOnly({ "/World/Block_1/B_0" }) },
        { withRules({ { "/", SceneLoadSet::eRuleAll }, { "/World/Block_1", SceneLoadSet::eRuleNone } }),
          withRules({ { "/", SceneLoadSet::eRuleNone }, { "/World/Block_1/B_0", SceneLoadSet::eRuleAll } }) },
        { withRules({ { "/", SceneLoadSet::eRuleNone }, { building, SceneLoadSet::eRuleOnly } }),
          withRules({ { "/", SceneLoadSet::eRuleNone }, { building, SceneLoadSet::eRuleNone }, { building + "/car_0", SceneLoadSet::eRuleAll } }) },
        { withRules({ { "/World", SceneLoadSet::eRuleOnly } }), withRules({ { "/World/Block_0", SceneLoadSet::eRuleAll }, { "/World/Block_1", SceneLoadSet::eRuleNone } }) },
    };

    SceneQueryPool::Lease lease = pool.borrow(city.payload(), SceneLoadSetPtr(), &error);
    ASSERT_TRUE(lease.isValid()) << error;
    std::vector<PXR_NS::SdfPath> prims;
    for (const PXR_NS::UsdPrim& prim : lease.getStage()->TraverseAll()) {
        prims.push_back(prim.GetPath());
    }
    ASSERT_GT(prims.size(), (std::size_t)city.getNumBuildings());

    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const SceneLoadSetPtr united = SceneLoadSet::unite(pairs[i].first, pairs[i].second);
        const PXR_NS::UsdStageLoadRules a = SceneOps::getLoadRules(pairs[i].first.get());
        const PXR_NS::UsdStageLoadRules b = SceneOps::getLoadRules(pairs[i].second.get());
        const PXR_NS::UsdStageLoadRules u = SceneOps::getLoadRules(united.get());
        int loaded = 0;
        for (const PXR_NS::SdfPath& path : prims) {
            const bool either = a.IsLoaded(path) || b.IsLoaded(path);
            EXPECT_EQ(u.IsLoaded(path), either) << "pair " << i << " at " << path.GetString();
            loaded += either ? 1 : 0;
        }
        EXPECT_LT(loaded, (int)prims.size()) << "pair " << i;
        EXPECT_EQ(*united, *SceneLoadSet::unite(pairs[i].second, pairs[i].first)) << "pair " << i;
    }
}

TEST(SceneLoadSetUnite, EverythingWins)
{
    const SceneLoadSetPtr some = loadingOnly({ "/World/Block_0" });

    EXPECT_FALSE(SceneLoadSet::unite(SceneLoadSetPtr(), some));
    EXPECT_FALSE(SceneLoadSet::unite(some, SceneLoadSetPtr()));
    EXPECT_FALSE(SceneLoadSet::unite(some, std::make_shared<const SceneLoadSet>()));
    EXPECT_FALSE(SceneLoadSet::unite(withRules({ { "/World", SceneLoadSet::eRuleNone } }), withRules({ { "/World", SceneLoadSet::eRuleAll } })));
    const SceneLoadSetPtr same = SceneLoadSet::unite(some, loadingOnly({ "/World/Block_0" }));
    ASSERT_TRUE(bool(same));
    EXPECT_EQ(*same, *some);
}

class SceneInputContextTest
    : public BaseTest {
protected:
    std::shared_ptr<SceneTestContextProbe> probe(NodePtr* node,
                                                 const NodePtr& inputA = NodePtr(),
                                                 const NodePtr& inputB = NodePtr())
    {
        *node = createNode(QString::fromUtf8(kTestPluginIDSceneContextProbe));
        EXPECT_TRUE(bool(*node));
        if (!*node) {
            return std::shared_ptr<SceneTestContextProbe>();
        }
        if (inputA) {
            EXPECT_TRUE((*node)->connectInput(inputA, 0));
        }
        if (inputB) {
            EXPECT_TRUE((*node)->connectInput(inputB, 1));
        }

        return std::dynamic_pointer_cast<SceneTestContextProbe>((*node)->getEffectInstance());
    }

    static void evaluate(const NodePtr& node,
                         const SceneEvaluationContext& context)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0), context), &payload, &error), EffectInstance::eRenderRoIRetCodeOk) << error;
    }

    static SceneEvaluationContext viewerContext()
    {
        return SceneEvaluationContext(SceneEvaluationContext::makeId(), loadingOnly({ "/Grp/World/Block_0" }), std::make_shared<SceneLoadReach>());
    }
};

TEST_F(SceneInputContextTest, AnIdentityNodePassesItsOwnContextOn)
{
    NodePtr sourceNode, identityNode, equalNode;
    std::shared_ptr<SceneTestContextProbe> source = probe(&sourceNode);
    std::shared_ptr<SceneTestContextProbe> identity = probe(&identityNode, sourceNode);
    ASSERT_TRUE(source && identity);
    const SceneEvaluationContext context = viewerContext();

    evaluate(identityNode, context);
    std::vector<SceneEvaluationContext> seen = source->getSeenContexts();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].id, context.id);
    EXPECT_EQ(seen[0].loadSet.get(), context.loadSet.get());
    EXPECT_EQ(seen[0].reach.get(), context.reach.get());
    const std::vector<SceneEvaluationContext> inputs = identity->getSeenInputContexts();
    ASSERT_FALSE(inputs.empty());
    EXPECT_EQ(inputs[0].loadSet.get(), context.loadSet.get());
    EXPECT_EQ(inputs[0].reach.get(), context.reach.get());

    // An equal set, not the same object, maps nothing either.
    std::shared_ptr<SceneTestContextProbe> equal = probe(&equalNode, sourceNode);
    ASSERT_TRUE(bool(equal));
    equal->inputLoadSet = [](const SceneLoadSetPtr&) { return loadingOnly({ "/Grp/World/Block_0" }); };
    source->clearSeen();
    evaluate(equalNode, context);
    seen = source->getSeenContexts();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].id, context.id);
    EXPECT_EQ(seen[0].loadSet.get(), context.loadSet.get());
    EXPECT_EQ(seen[0].reach.get(), context.reach.get());
}

TEST_F(SceneInputContextTest, AMappedInputsReachComesBackMapped)
{
    NodePtr sourceNode, mappingNode;
    std::shared_ptr<SceneTestContextProbe> source = probe(&sourceNode);
    std::shared_ptr<SceneTestContextProbe> mapping = probe(&mappingNode, sourceNode);
    ASSERT_TRUE(source && mapping);
    const SceneLoadSetPtr inputSet = loadingOnly({ "/World/Block_0" });
    source->reachToAdd = { "/World/Block_0/B_1" };
    mapping->inputLoadSet = [inputSet](const SceneLoadSetPtr&) { return inputSet; };
    mapping->reachPrefix = "/Grp";
    const SceneEvaluationContext context = viewerContext();

    evaluate(mappingNode, context);
    const std::vector<SceneEvaluationContext> seen = source->getSeenContexts();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_NE(seen[0].id, context.id);
    EXPECT_NE(seen[0].id, 0u);
    ASSERT_TRUE(bool(seen[0].loadSet));
    EXPECT_EQ(*seen[0].loadSet, *inputSet);
    ASSERT_TRUE(bool(seen[0].reach));
    EXPECT_NE(seen[0].reach.get(), context.reach.get());
    EXPECT_EQ(seen[0].reach->getPrefixes(), std::vector<std::string>({ "/World/Block_0/B_1" }));
    EXPECT_EQ(context.reach->getPrefixes(), std::vector<std::string>({ "/Grp/World/Block_0/B_1" }));
    const std::vector<SceneEvaluationContext> inputs = mapping->getSeenInputContexts();
    ASSERT_FALSE(inputs.empty());
    EXPECT_EQ(inputs[0].id, seen[0].id);
    EXPECT_EQ(inputs[0].reach.get(), seen[0].reach.get());

    source->clearSeen();
    evaluate(mappingNode, context);
    ASSERT_EQ(source->getSeenContexts().size(), 1u);
    EXPECT_EQ(source->getSeenContexts()[0].id, seen[0].id);

    // Without a reach to gather into, the input gathers none.
    source->clearSeen();
    evaluate(mappingNode, SceneEvaluationContext(context.id, context.loadSet));
    ASSERT_EQ(source->getSeenContexts().size(), 1u);
    EXPECT_EQ(source->getSeenContexts()[0].id, seen[0].id);
    EXPECT_FALSE(source->getSeenContexts()[0].reach);
}

TEST_F(SceneInputContextTest, ANodeReachedThroughAMappingAndDirectlyKeepsTwoSlots)
{
    NodePtr sourceNode, mappingNode, topNode;
    std::shared_ptr<SceneTestContextProbe> source = probe(&sourceNode);
    std::shared_ptr<SceneTestContextProbe> mapping = probe(&mappingNode, sourceNode);
    std::shared_ptr<SceneTestContextProbe> top = probe(&topNode, mappingNode, sourceNode);
    ASSERT_TRUE(source && mapping && top);
    mapping->inputLoadSet = [](const SceneLoadSetPtr&) { return loadingOnly({ "/World/Block_0" }); };
    const SceneEvaluationContext context = viewerContext();

    std::vector<U64> firstIds;
    for (int pass = 0; pass < 2; ++pass) {
        source->clearSeen();
        evaluate(topNode, context);
        std::vector<U64> ids;
        for (const SceneEvaluationContext& seen : source->getSeenContexts()) {
            ids.push_back(seen.id);
        }
        std::sort(ids.begin(), ids.end());
        ASSERT_EQ(ids.size(), 2u);
        EXPECT_NE(ids[0], ids[1]);
        EXPECT_TRUE(std::find(ids.begin(), ids.end(), context.id) != ids.end());
        if (pass == 0) {
            firstIds = ids;
        } else {
            EXPECT_EQ(ids, firstIds);
        }
    }
}

TEST(SceneQueryPoolLoadSets, WithPathsKeepsWhatABaseWithNoRootRuleLoads)
{
    CarCity city(2, 3);
    SceneQueryPool pool(1);
    std::string error;
    const SceneLoadSetPtr unloadedBlock = SceneLoadSet::minimal({ { "/World/Block_1", SceneLoadSet::eRuleNone } });
    const SceneLoadSetPtr united = SceneLoadSet::unite(loadingOnly({ "/World/Block_0" }), withRules({ { "/World/Block_1", SceneLoadSet::eRuleNone } }));

    for (const SceneLoadSetPtr& base : { unloadedBlock, united }) {
        ASSERT_TRUE(bool(base));
        ASSERT_TRUE(std::none_of(base->getEntries().begin(), base->getEntries().end(), [](const SceneLoadSet::Entry& entry) {
            return entry.path == "/";
        }));
        const SceneLoadSetPtr set = SceneLoadSet::withPaths(base, { carPath(1, 0) });
        ASSERT_TRUE(bool(set));
        {
            SceneQueryPool::Lease lease = pool.borrow(city.payload(), set, &error);
            ASSERT_TRUE(lease.isValid()) << error;
            EXPECT_EQ(city.openedBuildings(), 4);
            EXPECT_TRUE(lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_0/B_2")).IsLoaded());
            EXPECT_TRUE(lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_1/B_0")).IsLoaded());
            EXPECT_FALSE(lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/Block_1/B_1")).IsLoaded());
        }
        pool.releaseIdle();
    }
}

TEST(SceneQueryPoolLoadSets, AnOnlyRuleUnderEverythingKeepsItsSiblingsUnloaded)
{
    CarCity city(2, 3);
    SceneQueryPool pool(1);
    std::string error;
    const std::vector<SceneLoadSetPtr> sets = {
        withRules({ { "/World/Block_0", SceneLoadSet::eRuleOnly }, { "/World/Block_0/B_1", SceneLoadSet::eRuleAll } }),
        withRules({ { "/", SceneLoadSet::eRuleAll }, { "/World/Block_0", SceneLoadSet::eRuleOnly }, { "/World/Block_0/B_1", SceneLoadSet::eRuleAll } }),
    };

    for (std::size_t i = 0; i < sets.size(); ++i) {
        PXR_NS::UsdStageLoadRules rules = SceneOps::getLoadRules(sets[i].get());
        PXR_NS::UsdStageLoadRules minimized = rules;
        minimized.Minimize();
        for (const PXR_NS::UsdStageLoadRules* r : { &rules, &minimized }) {
            EXPECT_TRUE(r->IsLoaded(PXR_NS::SdfPath("/World/Block_0/B_1"))) << "set " << i;
            EXPECT_FALSE(r->IsLoaded(PXR_NS::SdfPath("/World/Block_0/B_0"))) << "set " << i;
            EXPECT_TRUE(r->IsLoaded(PXR_NS::SdfPath("/World/Block_1/B_0"))) << "set " << i;
        }
        {
            SceneQueryPool::Lease lease = pool.borrow(city.payload(), sets[i], &error);
            ASSERT_TRUE(lease.isValid()) << error;
            EXPECT_EQ(city.openedBuildings(), 4) << "set " << i;
        }
        pool.releaseIdle();
    }
}
