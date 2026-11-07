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
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/base/tf/weakBase.h>
#include <pxr/base/tf/weakPtr.h>
#include <pxr/usd/usd/primRange.h>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
PXR_NS::SdfLayerRefPtr
makeLayer(const std::string& body,
          const std::string& header = std::string())
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneStageTest");

    EXPECT_TRUE(layer->ImportFromString("#usda 1.0\n" + header + "\n" + body));

    return layer;
}

ScenePayload
stack(const std::vector<PXR_NS::SdfLayerRefPtr>& layers)
{
    ScenePayload payload;
    U64 hash = 1;

    for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
        payload = SceneOps::appendLayer(payload, layer, hash++);
    }

    return payload;
}

// Every prim, its type and every attribute's value at the default time and at time 1, plus the
// stage metadata, as text: two stages composing the same scene give the same text.
std::string
describe(const PXR_NS::UsdStageRefPtr& stage)
{
    std::ostringstream out;
    PXR_NS::TfToken upAxis;

    stage->GetMetadata(PXR_NS::UsdGeomTokens->upAxis, &upAxis);
    out << "upAxis=" << upAxis << " defaultPrim=" << stage->GetDefaultPrim().GetPath()
        << " tcps=" << stage->GetTimeCodesPerSecond() << "\n";
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        out << prim.GetPath() << " " << prim.GetTypeName() << "\n";
        for (const PXR_NS::UsdAttribute& attribute : prim.GetAttributes()) {
            PXR_NS::VtValue atDefault, atOne;
            attribute.Get(&atDefault);
            attribute.Get(&atOne, PXR_NS::UsdTimeCode(1.));
            out << "  " << attribute.GetName() << " " << atDefault << " " << atOne << "\n";
        }
    }

    return out.str();
}

// Records the ObjectsChanged notices one stage sends.
class NoticeLog
    : public PXR_NS::TfWeakBase {
public:
    explicit NoticeLog(const PXR_NS::UsdStageRefPtr& stage)
    {
        _key = PXR_NS::TfNotice::Register(PXR_NS::TfCreateWeakPtr(this), &NoticeLog::onObjectsChanged,
                                          PXR_NS::UsdStagePtr(stage));
    }

    ~NoticeLog()
    {
        PXR_NS::TfNotice::Revoke(_key);
    }

    void onObjectsChanged(const PXR_NS::UsdNotice::ObjectsChanged& notice)
    {
        ++notices;
        for (const PXR_NS::SdfPath& path : notice.GetResyncedPaths()) {
            resynced.push_back(path);
        }
        for (const PXR_NS::SdfPath& path : notice.GetChangedInfoOnlyPaths()) {
            for (const PXR_NS::TfToken& field : notice.GetChangedFields(path)) {
                if (path == PXR_NS::SdfPath::AbsoluteRootPath()) {
                    rootFields.push_back(field);
                }
            }
        }
    }

    void clear()
    {
        notices = 0;
        resynced.clear();
        rootFields.clear();
    }

    bool rootFieldChanged(const PXR_NS::TfToken& field) const
    {
        return std::find(rootFields.begin(), rootFields.end(), field) != rootFields.end();
    }

    int notices = 0;
    std::vector<PXR_NS::SdfPath> resynced;
    std::vector<PXR_NS::TfToken> rootFields;

private:
    PXR_NS::TfNotice::Key _key;
};

const char* const kZUp = "(\n    upAxis = \"Z\"\n    metersPerUnit = 1\n)\n";
} // anonymous namespace

TEST(SceneStageTest, StartsEmptyAndOpensOnce)
{
    const U64 opensBefore = SceneOps::getStageOpenCount();
    SceneStage stage;

    ASSERT_TRUE(stage.isValid());
    EXPECT_EQ(opensBefore + 1, SceneOps::getStageOpenCount());
    EXPECT_TRUE(stage.getPayload().isEmpty());
    EXPECT_EQ(describe(SceneOps::compose(ScenePayload())), describe(stage.getStage()));
    const U64 opensAfterCompose = SceneOps::getStageOpenCount();

    const PXR_NS::UsdStageRefPtr first = stage.getStage();
    stage.sync(stack({ makeLayer("def Xform \"A\"\n{\n}\n") }));
    EXPECT_EQ(first, stage.getStage());
    EXPECT_EQ(opensAfterCompose, SceneOps::getStageOpenCount()) << "sync opens no stage";
}

// Swapping one node's layer for another notifies only the paths those two layers author.
TEST(SceneStageTest, EditingOneLayerResyncsOnlyItsPaths)
{
    const PXR_NS::SdfLayerRefPtr a = makeLayer("def Sphere \"A\"\n{\n    double radius = 1\n}\n", kZUp);
    const PXR_NS::SdfLayerRefPtr b = makeLayer("def Cube \"B\"\n{\n    double size = 1\n}\n");
    const PXR_NS::SdfLayerRefPtr edited = makeLayer("def Cube \"B\"\n{\n    double size = 2\n}\n\ndef Xform \"BExtra\"\n{\n}\n");
    const PXR_NS::SdfLayerRefPtr c = makeLayer("over \"A\"\n{\n    double radius = 3\n}\n");
    SceneStage stage;

    ASSERT_TRUE(stage.isValid());
    EXPECT_TRUE(stage.sync(stack({ a, b, c })));

    NoticeLog log(stage.getStage());
    const ScenePayload after = stack({ a, edited, c });
    EXPECT_TRUE(stage.sync(after));

    EXPECT_EQ(1, log.notices);
    ASSERT_FALSE(log.resynced.empty());
    for (const PXR_NS::SdfPath& path : log.resynced) {
        EXPECT_NE(PXR_NS::SdfPath::AbsoluteRootPath(), path);
        EXPECT_TRUE(path.HasPrefix(PXR_NS::SdfPath("/B")) || path.HasPrefix(PXR_NS::SdfPath("/BExtra"))) << path;
    }
    // The metadata did not change, so the root's metadata fields were not rewritten.
    EXPECT_FALSE(log.rootFieldChanged(PXR_NS::UsdGeomTokens->upAxis));
    EXPECT_FALSE(log.rootFieldChanged(PXR_NS::UsdGeomTokens->metersPerUnit));
    EXPECT_EQ(describe(SceneOps::compose(after)), describe(stage.getStage()));
}

TEST(SceneStageTest, SyncingTheAppliedStackSendsNothing)
{
    const PXR_NS::SdfLayerRefPtr a = makeLayer("def Sphere \"A\"\n{\n}\n", kZUp);
    const PXR_NS::SdfLayerRefPtr b = makeLayer("over \"A\"\n{\n    double radius = 2\n}\n");
    SceneStage stage;

    ASSERT_TRUE(stage.isValid());
    stage.sync(stack({ a, b }));
    const U64 edits = stage.getEditCount();

    NoticeLog log(stage.getStage());
    // Another payload object over the same layers.
    EXPECT_FALSE(stage.sync(stack({ a, b })));
    EXPECT_EQ(0, log.notices);
    EXPECT_EQ(edits, stage.getEditCount());
}

// Metadata is written only when it changes, and a change notifies / as changed info, never as a
// resync.
TEST(SceneStageTest, MetadataIsWrittenOnlyWhenItChanges)
{
    const PXR_NS::SdfLayerRefPtr yUp = makeLayer("def Sphere \"A\"\n{\n}\n");
    const PXR_NS::SdfLayerRefPtr zUp = makeLayer("", "(\n    upAxis = \"Z\"\n)\n");
    SceneStage stage;

    ASSERT_TRUE(stage.isValid());
    stage.sync(stack({ yUp }));

    NoticeLog log(stage.getStage());
    const ScenePayload withZ = stack({ yUp, zUp });
    EXPECT_TRUE(stage.sync(withZ));
    EXPECT_TRUE(log.rootFieldChanged(PXR_NS::UsdGeomTokens->upAxis));
    EXPECT_FALSE(log.rootFieldChanged(PXR_NS::UsdGeomTokens->metersPerUnit));
    for (const PXR_NS::SdfPath& path : log.resynced) {
        EXPECT_NE(PXR_NS::SdfPath::AbsoluteRootPath(), path);
    }
    EXPECT_EQ(PXR_NS::UsdGeomTokens->z, PXR_NS::UsdGeomGetStageUpAxis(stage.getStage()));
    EXPECT_EQ(describe(SceneOps::compose(withZ)), describe(stage.getStage()));
}

// Chains, a merge of two branches and a diamond sharing one base, synced one after the other on
// the same stage: each time the stage reads the same as a freshly composed one.
TEST(SceneStageTest, MatchesAFreshlyComposedStage)
{
    const PXR_NS::SdfLayerRefPtr base = makeLayer("def Xform \"World\"\n{\n    def Sphere \"Ball\"\n    {\n        double radius = 1\n    }\n}\n", kZUp);
    const PXR_NS::SdfLayerRefPtr left = makeLayer("over \"World\"\n{\n    over \"Ball\"\n    {\n        double radius.timeSamples = {\n            1: 2,\n            10: 4,\n        }\n    }\n}\n");
    const PXR_NS::SdfLayerRefPtr right = makeLayer("over \"World\"\n{\n    def Cube \"Box\"\n    {\n        double size = 5\n    }\n}\n",
                                                   "(\n    defaultPrim = \"World\"\n    timeCodesPerSecond = 30\n)\n");
    const PXR_NS::SdfLayerRefPtr top = makeLayer("over \"World\"\n{\n    over \"Ball\"\n    {\n        double radius = 7\n    }\n}\n");
    const PXR_NS::SdfLayerRefPtr other = makeLayer("def Xform \"Other\"\n{\n}\n");

    const std::vector<ScenePayload> payloads = {
        stack({ base }),
        stack({ base, left }), // chain
        stack({ base, left, top }), // longer chain
        stack({ other, base, right }), // merge of two branches
        stack({ base, left, right }), // diamond: both branches over one base
        stack({ base, right, left, top }), // reordered
        stack({ base }), // back to a prefix
        ScenePayload(), // empty
        stack({ base, left, right }),
    };

    SceneStage stage;
    ASSERT_TRUE(stage.isValid());
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        stage.sync(payloads[i]);
        EXPECT_EQ(describe(SceneOps::compose(payloads[i])), describe(stage.getStage())) << "payload " << i;
        EXPECT_TRUE(stage.getPayload().isSameStack(payloads[i]));
    }
}

TEST(SceneStageTest, CommonBaseLengthCountsSharedWeakestLayers)
{
    const PXR_NS::SdfLayerRefPtr a = makeLayer("def Xform \"A\"\n{\n}\n");
    const PXR_NS::SdfLayerRefPtr b = makeLayer("def Xform \"B\"\n{\n}\n");
    const PXR_NS::SdfLayerRefPtr c = makeLayer("def Xform \"C\"\n{\n}\n");
    SceneStage stage;

    ASSERT_TRUE(stage.isValid());
    EXPECT_EQ((std::size_t)0, stage.getCommonBaseLength(stack({ a })));
    stage.sync(stack({ a, b }));
    EXPECT_EQ((std::size_t)2, stage.getCommonBaseLength(stack({ a, b, c })));
    EXPECT_EQ((std::size_t)1, stage.getCommonBaseLength(stack({ a, c })));
    EXPECT_EQ((std::size_t)0, stage.getCommonBaseLength(stack({ b, a })));
}

namespace {
// A small city on disk: /World/Block_<b>/B_<n>, one payload file per building, the shape of what
// tools/bench/heavy_scene_make.py writes.
class PayloadCity {
public:
    PayloadCity(int blocks,
                int buildingsPerBlock)
    {
        static std::atomic<int> counter(0);

        _dir = std::filesystem::temp_directory_path() / ("scene_stage_city_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(_dir / "buildings");
        // Compared with what SdfLayer::GetRealPath() gives.
        _dir = std::filesystem::canonical(_dir);
        std::ofstream root((_dir / "root.usda").string());
        root << "#usda 1.0\n(\n    defaultPrim = \"World\"\n)\ndef Xform \"World\"\n{\n";
        for (int b = 0; b < blocks; ++b) {
            root << "    def Xform \"Block_" << b << "\"\n    {\n";
            for (int n = 0; n < buildingsPerBlock; ++n) {
                const std::string name = "b" + std::to_string(b) + "_" + std::to_string(n) + ".usda";
                writeBuilding(name, std::string());
                root << "        def Xform \"B_" << n << "\" (\n            prepend payload = @./buildings/" << name << "@\n        )\n        {\n        }\n";
            }
            root << "    }\n";
        }
        root << "}\n";
    }

    ~PayloadCity()
    {
        std::error_code ignored;
        std::filesystem::remove_all(_dir, ignored);
    }

    // Replaces a building's file, with extra as more of its root prim's body.
    void writeBuilding(const std::string& name,
                       const std::string& extra) const
    {
        std::ofstream building(buildingPath(name));
        building << "#usda 1.0\n(\n    defaultPrim = \"Building\"\n)\ndef Xform \"Building\"\n{\n    def Cube \"Geo\"\n    {\n    }\n"
                 << extra << "}\n";
    }

    std::string buildingPath(const std::string& name) const
    {
        return (_dir / "buildings" / name).string();
    }

    void removeBuilding(const std::string& name) const
    {
        std::filesystem::remove(buildingPath(name));
    }

    ScenePayload payload() const
    {
        const PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::FindOrOpen((_dir / "root.usda").string());

        EXPECT_TRUE(bool(root));

        return stack({ root });
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

const SceneStage::LockedSection kNoLock = [](const std::function<void()>& section) { section(); };

PXR_NS::SdfPath
building(int block,
         int index)
{
    return PXR_NS::SdfPath("/World/Block_" + std::to_string(block) + "/B_" + std::to_string(index));
}
} // anonymous namespace

TEST(SceneStageLoads, OpenedUnloadedLeavesEveryBuildingClosed)
{
    PayloadCity city(2, 3);
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.isValid());
    ASSERT_TRUE(stage.sync(city.payload()));

    EXPECT_TRUE(city.loadedBuildings().empty());
    const PXR_NS::UsdPrim row = stage.getStage()->GetPrimAtPath(building(0, 0));
    ASSERT_TRUE(bool(row));
    EXPECT_TRUE(row.HasAuthoredPayloads());
    EXPECT_FALSE(row.IsLoaded());
    EXPECT_FALSE(bool(stage.getStage()->GetPrimAtPath(building(0, 0).AppendChild(PXR_NS::TfToken("Geo")))));

    // The policy outlives the first sync.
    ASSERT_TRUE(stage.sync(stack({ makeLayer("def Xform \"Other\"\n{\n}\n") })));
    ASSERT_TRUE(stage.sync(city.payload()));
    EXPECT_TRUE(city.loadedBuildings().empty());
}

TEST(SceneStageLoads, DefaultStageStaysFullyLoaded)
{
    PayloadCity city(1, 2);
    SceneStage stage;
    ASSERT_TRUE(stage.sync(city.payload()));

    EXPECT_EQ(city.loadedBuildings(), std::vector<std::string>({ "b0_0.usda", "b0_1.usda" }));
    EXPECT_TRUE(bool(stage.getStage()->GetPrimAtPath(building(0, 1).AppendChild(PXR_NS::TfToken("Geo")))));
}

TEST(SceneStageLoads, RequestsBatchIntoOneLoadAndUnloadNamingOnlyTheirSubtrees)
{
    PayloadCity city(2, 3);
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(city.payload()));
    NoticeLog log(stage.getStage());

    stage.requestLoad({ building(0, 0) }, false);
    stage.requestLoad({ building(1, 2) }, false);
    EXPECT_TRUE(stage.hasPendingLoads());

    std::vector<std::string> openBeforeTheLock;
    const SceneStage::LockedSection writeLocked = [&](const std::function<void()>& section) {
        openBeforeTheLock = city.loadedBuildings();
        section();
    };
    EXPECT_TRUE(stage.serviceLoads(kNoLock, writeLocked));

    EXPECT_FALSE(stage.hasPendingLoads());
    EXPECT_EQ(stage.getLoadEditCount(), U64(1));
    EXPECT_EQ(openBeforeTheLock, std::vector<std::string>({ "b0_0.usda", "b1_2.usda" }));
    EXPECT_EQ(city.loadedBuildings(), openBeforeTheLock);
    EXPECT_GE(stage.getLastLoadHoldMs(), 0.);

    EXPECT_EQ(log.notices, 1);
    std::vector<PXR_NS::SdfPath> resynced = log.resynced;
    std::sort(resynced.begin(), resynced.end());
    EXPECT_EQ(resynced, std::vector<PXR_NS::SdfPath>({ building(0, 0), building(1, 2) }));

    EXPECT_TRUE(bool(stage.getStage()->GetPrimAtPath(building(0, 0).AppendChild(PXR_NS::TfToken("Geo")))));
    EXPECT_FALSE(stage.getStage()->GetPrimAtPath(building(0, 1)).IsLoaded());
    // Edits of the layer stack and loads are told apart.
    EXPECT_EQ(stage.getEditCount(), U64(1));
}

TEST(SceneStageLoads, RequestDuringABatchGoesToTheNext)
{
    PayloadCity city(1, 3);
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(city.payload()));

    stage.requestLoad({ building(0, 0) }, false);
    const SceneStage::LockedSection writeLocked = [&](const std::function<void()>& section) {
        stage.requestLoad({ building(0, 1) }, false);
        section();
    };
    EXPECT_TRUE(stage.serviceLoads(kNoLock, writeLocked));
    EXPECT_TRUE(stage.hasPendingLoads());
    EXPECT_FALSE(stage.getStage()->GetPrimAtPath(building(0, 1)).IsLoaded());

    EXPECT_TRUE(stage.serviceLoads(kNoLock, kNoLock));
    EXPECT_TRUE(stage.getStage()->GetPrimAtPath(building(0, 1)).IsLoaded());
    EXPECT_EQ(stage.getLoadEditCount(), U64(2));
}

TEST(SceneStageLoads, LoadingWhatIsLoadedDoesNoWork)
{
    PayloadCity city(1, 2);
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(city.payload()));
    stage.requestLoad({ building(0, 0) }, false);
    stage.serviceLoads(kNoLock, kNoLock);
    NoticeLog log(stage.getStage());

    stage.requestLoad({ building(0, 0) }, false);
    int writes = 0;
    const SceneStage::LockedSection writeLocked = [&](const std::function<void()>& section) {
        ++writes;
        section();
    };
    EXPECT_TRUE(stage.serviceLoads(kNoLock, writeLocked));

    EXPECT_EQ(writes, 0);
    EXPECT_EQ(stage.getLoadEditCount(), U64(1));
    EXPECT_EQ(log.notices, 0);
}

TEST(SceneStageLoads, UnloadTakesTheSubtreeAwayAndKeepsThePayloadPrim)
{
    PayloadCity city(1, 2);
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(city.payload()));
    stage.requestLoad({ building(0, 0), building(0, 1) }, false);
    stage.serviceLoads(kNoLock, kNoLock);
    NoticeLog log(stage.getStage());

    stage.requestUnload({ building(0, 0) });
    EXPECT_TRUE(stage.serviceLoads(kNoLock, kNoLock));

    const PXR_NS::UsdPrim row = stage.getStage()->GetPrimAtPath(building(0, 0));
    ASSERT_TRUE(bool(row));
    EXPECT_FALSE(row.IsLoaded());
    EXPECT_FALSE(bool(stage.getStage()->GetPrimAtPath(building(0, 0).AppendChild(PXR_NS::TfToken("Geo")))));
    EXPECT_TRUE(stage.getStage()->GetPrimAtPath(building(0, 1)).IsLoaded());
    EXPECT_EQ(log.resynced, std::vector<PXR_NS::SdfPath>({ building(0, 0) }));
}

TEST(SceneStageLoads, ALaterRequestForAPathReplacesAnEarlierOne)
{
    PayloadCity city(1, 1);
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(city.payload()));

    stage.requestLoad({ building(0, 0) }, false);
    stage.requestUnload({ building(0, 0) });
    stage.serviceLoads(kNoLock, kNoLock);

    EXPECT_FALSE(stage.getStage()->GetPrimAtPath(building(0, 0)).IsLoaded());
    EXPECT_TRUE(city.loadedBuildings().empty());
}

TEST(SceneStageLoads, WithDescendantsReachesEveryBuildingBelowThePathOnly)
{
    PayloadCity city(2, 2);
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(city.payload()));

    stage.requestLoad({ PXR_NS::SdfPath("/World/Block_0") }, true);
    EXPECT_TRUE(stage.serviceLoads(kNoLock, kNoLock));

    EXPECT_EQ(city.loadedBuildings(), std::vector<std::string>({ "b0_0.usda", "b0_1.usda" }));
    EXPECT_TRUE(stage.getStage()->GetPrimAtPath(building(0, 1)).IsLoaded());
    EXPECT_FALSE(stage.getStage()->GetPrimAtPath(building(1, 0)).IsLoaded());
}

TEST(SceneStageLoads, OwnLoadLeavesANestedPayloadClosed)
{
    PayloadCity city(1, 1);
    city.writeBuilding("nested.usda", std::string());
    city.writeBuilding("b0_0.usda", "    def Xform \"Wing\" (\n        prepend payload = @./nested.usda@\n    )\n    {\n    }\n");
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(city.payload()));

    stage.requestLoad({ building(0, 0) }, false);
    EXPECT_TRUE(stage.serviceLoads(kNoLock, kNoLock));

    EXPECT_EQ(city.loadedBuildings(), std::vector<std::string>({ "b0_0.usda" }));
    const PXR_NS::UsdPrim wing = stage.getStage()->GetPrimAtPath(building(0, 0).AppendChild(PXR_NS::TfToken("Wing")));
    ASSERT_TRUE(bool(wing));
    EXPECT_FALSE(wing.IsLoaded());
}

TEST(SceneStageLoads, AFileThatDoesNotOpenIsReportedAndTheRestLoads)
{
    PayloadCity city(1, 2);
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(city.payload()));
    city.removeBuilding("b0_0.usda");

    stage.requestLoad({ building(0, 0), building(0, 1) }, false);
    EXPECT_TRUE(stage.serviceLoads(kNoLock, kNoLock));

    EXPECT_FALSE(stage.getStage()->GetPrimAtPath(building(0, 0)).IsLoaded());
    EXPECT_TRUE(stage.getStage()->GetPrimAtPath(building(0, 1)).IsLoaded());
    const std::vector<SceneStage::LoadFailure> failures = stage.getLoadFailures();
    ASSERT_EQ(failures.size(), std::size_t(1));
    EXPECT_EQ(failures[0].prim, building(0, 0));
    EXPECT_FALSE(failures[0].error.empty());

    // Asking again tries again.
    city.writeBuilding("b0_0.usda", std::string());
    stage.requestLoad({ building(0, 0) }, false);
    EXPECT_TRUE(stage.serviceLoads(kNoLock, kNoLock));
    EXPECT_TRUE(stage.getLoadFailures().empty());
    EXPECT_TRUE(stage.getStage()->GetPrimAtPath(building(0, 0)).IsLoaded());
}

TEST(SceneStageLoads, CancelDropsWhatHasNotStarted)
{
    PayloadCity city(1, 1);
    SceneStage stage(PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage.sync(city.payload()));

    stage.requestLoad({ building(0, 0) }, false);
    stage.cancelPendingLoads();

    EXPECT_FALSE(stage.hasPendingLoads());
    EXPECT_FALSE(stage.serviceLoads(kNoLock, kNoLock));
    EXPECT_TRUE(city.loadedBuildings().empty());
}
