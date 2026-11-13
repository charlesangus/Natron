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

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"
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
