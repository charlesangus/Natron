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

    const PXR_NS::UsdStageRefPtr first = stage.getStage();
    stage.sync(stack({ makeLayer("def Xform \"A\"\n{\n}\n") }));
    EXPECT_EQ(first, stage.getStage());
    EXPECT_EQ(opensBefore + 1, SceneOps::getStageOpenCount());
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
