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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/base/tf/weakBase.h>
#include <pxr/base/tf/weakPtr.h>

#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
PXR_NS::SdfLayerRefPtr
makeLayer(const std::string& body,
          const std::string& header = std::string())
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneReparentTest");

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

const char* const kCard = "def Xform \"Card1\"\n{\n"
                          "    double3 xformOp:translate.timeSamples = {\n        1: (0, 0, 0),\n        10: (9, 0, 0),\n    }\n"
                          "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                          "    def Mesh \"Geo\"\n    {\n    }\n}\n";

std::string
sphere(double radius)
{
    return "def Sphere \"Sphere1\"\n{\n    double radius = " + std::to_string(radius) + "\n}\n";
}

SceneReparentArgs
graftUnderT1(const std::vector<ScenePayload>& inputs,
             bool autoRename = false)
{
    SceneReparentArgs args;

    args.inputs = inputs;
    args.destinationPath = "/T1";
    args.destinationType = "Xform";
    args.ancestorType = "Xform";
    args.autoRename = autoRename;
    args.layerTag = "SceneReparentTest";

    return args;
}

// Authors /T1's translate as an "over", memoized on the value.
void
setTranslateOverrides(SceneReparentArgs* args,
                      double x)
{
    args->overridesKey.nodeDigest = (U64)(x * 1000.) + 1;
    args->overridesKey.readsFrameRange = false;
    args->overridesKey.framesPerSecond = 24.;
    args->authorOverrides = [x](const SceneLayerEdit& edit, std::string* /*errorMessage*/) {
        return edit.layer->ImportFromString("#usda 1.0\nover \"T1\"\n{\n    double3 xformOp:translate = (" + std::to_string(x) + ", 0, 0)\n    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n");
    };
}

ScenePayload
reparent(const SceneReparentArgs& args,
         SceneGraftMemo* memo,
         std::vector<SceneRootGraft>* grafts = NULL)
{
    ScenePayload output;
    std::string error;

    EXPECT_TRUE(SceneOps::reparentRoots(args, memo, &output, grafts, &error)) << error;

    return output;
}

std::vector<std::string>
rootPrimNames(const PXR_NS::UsdStageRefPtr& stage)
{
    std::vector<std::string> names;

    for (const PXR_NS::UsdPrim& root : stage->GetPseudoRoot().GetChildren()) {
        names.push_back(root.GetName().GetString());
    }

    return names;
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
    }

    int notices = 0;
    std::vector<PXR_NS::SdfPath> resynced;

private:
    PXR_NS::TfNotice::Key _key;
};
} // anonymous namespace

TEST(SceneReparentTest, GraftsEveryInputsRootsUnderTheDestination)
{
    const ScenePayload card = stack({ makeLayer(kCard) });
    const ScenePayload ball = stack({ makeLayer(sphere(1.)) });
    SceneGraftMemo memo;
    std::vector<SceneRootGraft> grafts;

    const ScenePayload result = reparent(graftUnderT1({ card, ball }), &memo, &grafts);
    // The structure layer, then one graft layer per input.
    EXPECT_EQ(3u, result.getNumLayers());

    const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(result);
    ASSERT_TRUE(stage);
    EXPECT_EQ(std::vector<std::string>({ "T1" }), rootPrimNames(stage));
    EXPECT_EQ(PXR_NS::TfToken("Xform"), stage->GetPrimAtPath(PXR_NS::SdfPath("/T1")).GetTypeName());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/T1/Card1/Geo")).IsValid());
    const PXR_NS::UsdPrim ballPrim = stage->GetPrimAtPath(PXR_NS::SdfPath("/T1/Sphere1"));
    ASSERT_TRUE(ballPrim.IsValid());
    EXPECT_EQ(PXR_NS::TfToken("Sphere"), ballPrim.GetTypeName());
    double radius = 0.;
    EXPECT_TRUE(ballPrim.GetAttribute(PXR_NS::TfToken("radius")).Get(&radius));
    EXPECT_EQ(1., radius);
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1")).IsValid());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Sphere1")).IsValid());

    ASSERT_EQ(2u, grafts.size());
    EXPECT_EQ(0u, grafts[0].inputIndex);
    EXPECT_EQ("/Card1", grafts[0].sourcePath);
    EXPECT_EQ("/T1/Card1", grafts[0].graftedPath);
    EXPECT_EQ(1u, grafts[1].inputIndex);
    EXPECT_EQ("/T1/Sphere1", grafts[1].graftedPath);
}

TEST(SceneReparentTest, AnimatedXformOpSurvivesTheGraft)
{
    SceneGraftMemo memo;
    const ScenePayload result = reparent(graftUnderT1({ stack({ makeLayer(kCard) }) }), &memo);
    const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(result);

    ASSERT_TRUE(stage);
    const PXR_NS::UsdGeomXformable card(stage->GetPrimAtPath(PXR_NS::SdfPath("/T1/Card1")));
    ASSERT_TRUE(card);
    bool resetsXformStack = false;
    const std::vector<PXR_NS::UsdGeomXformOp> ops = card.GetOrderedXformOps(&resetsXformStack);
    ASSERT_EQ(1u, ops.size());
    std::vector<double> times;
    EXPECT_TRUE(ops[0].GetTimeSamples(&times));
    EXPECT_EQ(std::vector<double>({ 1., 10. }), times);
    EXPECT_DOUBLE_EQ(9., card.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(10.)).ExtractTranslation()[0]);
    EXPECT_DOUBLE_EQ(4., card.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(5.)).ExtractTranslation()[0]);
}

TEST(SceneReparentTest, CollidingRootsComposeWithTheLaterInputStronger)
{
    const ScenePayload weak = stack({ makeLayer("def Mesh \"Card1\"\n{\n    custom int who = 1\n    custom int onlyWeak = 1\n}\n") });
    const ScenePayload strong = stack({ makeLayer("def Xform \"Card1\"\n{\n    custom int who = 2\n}\n") });
    SceneGraftMemo memo;
    const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(reparent(graftUnderT1({ weak, strong }), &memo));

    ASSERT_TRUE(stage);
    const PXR_NS::UsdPrim card = stage->GetPrimAtPath(PXR_NS::SdfPath("/T1/Card1"));
    ASSERT_TRUE(card.IsValid());
    int who = 0;
    int onlyWeak = 0;
    EXPECT_TRUE(card.GetAttribute(PXR_NS::TfToken("who")).Get(&who));
    EXPECT_TRUE(card.GetAttribute(PXR_NS::TfToken("onlyWeak")).Get(&onlyWeak));
    EXPECT_EQ(2, who);
    EXPECT_EQ(1, onlyWeak);
    EXPECT_EQ(PXR_NS::TfToken("Xform"), card.GetTypeName());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/T1/Card1_1")).IsValid());
}

TEST(SceneReparentTest, AutoRenameTakesTheFirstFreeSuffix)
{
    const ScenePayload first = stack({ makeLayer("def Xform \"Card1\"\n{\n    custom int who = 1\n}\n") });
    const ScenePayload second = stack({ makeLayer("def Xform \"Card1\"\n{\n    custom int who = 2\n}\n") });
    const ScenePayload third = stack({ makeLayer("def Xform \"Card1\"\n{\n    custom int who = 3\n}\n") });
    SceneGraftMemo memo;
    std::vector<SceneRootGraft> grafts;
    const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(reparent(graftUnderT1({ first, second, third }, true), &memo, &grafts));

    ASSERT_TRUE(stage);
    const char* const paths[] = { "/T1/Card1", "/T1/Card1_1", "/T1/Card1_2" };
    for (int i = 0; i < 3; ++i) {
        const PXR_NS::UsdPrim card = stage->GetPrimAtPath(PXR_NS::SdfPath(paths[i]));
        ASSERT_TRUE(card.IsValid()) << paths[i];
        int who = 0;
        EXPECT_TRUE(card.GetAttribute(PXR_NS::TfToken("who")).Get(&who));
        EXPECT_EQ(i + 1, who) << paths[i];
        ASSERT_LT((std::size_t)i, grafts.size());
        EXPECT_EQ("/Card1", grafts[i].sourcePath);
        EXPECT_EQ(paths[i], grafts[i].graftedPath);
    }
}

TEST(SceneReparentTest, NestedDestinationDefinesItsAncestors)
{
    SceneReparentArgs args = graftUnderT1({ stack({ makeLayer(kCard) }) });
    args.destinationPath = "/set/props/T1";
    args.ancestorType = "Scope";
    SceneGraftMemo memo;
    const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(reparent(args, &memo));

    ASSERT_TRUE(stage);
    EXPECT_EQ(PXR_NS::TfToken("Scope"), stage->GetPrimAtPath(PXR_NS::SdfPath("/set")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("Scope"), stage->GetPrimAtPath(PXR_NS::SdfPath("/set/props")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("Xform"), stage->GetPrimAtPath(PXR_NS::SdfPath("/set/props/T1")).GetTypeName());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/set/props/T1/Card1/Geo")).IsValid());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/set")).IsDefined());
}

TEST(SceneReparentTest, InputsWithoutRootsContributeNothing)
{
    const ScenePayload onlyOvers = stack({ makeLayer("over \"Elsewhere\"\n{\n}\n") });
    SceneGraftMemo memo;
    const ScenePayload result = reparent(graftUnderT1({ ScenePayload(), onlyOvers }), &memo);

    EXPECT_EQ(1u, result.getNumLayers());
    const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(result);
    ASSERT_TRUE(stage);
    EXPECT_EQ(std::vector<std::string>({ "T1" }), rootPrimNames(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/T1")).GetChildren().empty());
}

TEST(SceneReparentTest, InputRootAtTheDestinationIsAnError)
{
    SceneGraftMemo memo;
    const ScenePayload good = reparent(graftUnderT1({ stack({ makeLayer(kCard) }) }), &memo);
    ScenePayload output = good;
    std::string error;

    EXPECT_FALSE(SceneOps::reparentRoots(graftUnderT1({ stack({ makeLayer("def Xform \"T1\"\n{\n}\n") }) }), &memo, &output, NULL, &error));
    EXPECT_NE(std::string::npos, error.find("/T1")) << error;
    EXPECT_TRUE(output.isSameStack(good));

    SceneReparentArgs relative = graftUnderT1({});
    relative.destinationPath = "T1";
    EXPECT_FALSE(SceneOps::reparentRoots(relative, &memo, &output, NULL, &error));
}

TEST(SceneReparentTest, UnchangedInputsKeepTheirLayers)
{
    const ScenePayload card = stack({ makeLayer(kCard) });
    const ScenePayload ball = stack({ makeLayer(sphere(1.)) });
    SceneGraftMemo memo;
    SceneReparentArgs args = graftUnderT1({ card, ball });
    setTranslateOverrides(&args, 1.);

    const ScenePayload first = reparent(args, &memo);
    const U64 authored = memo.getGraftLayerAuthorCount();
    EXPECT_EQ(2u, authored);
    const ScenePayload second = reparent(args, &memo);

    EXPECT_TRUE(second.isSameStack(first));
    EXPECT_EQ(first.getImpl(), second.getImpl());
    EXPECT_EQ(authored, memo.getGraftLayerAuthorCount());

    // Another payload object holding the same layers is the same input.
    args.inputs = { ScenePayload(card.getImpl()), ScenePayload(ball.getImpl()) };
    EXPECT_TRUE(reparent(args, &memo).isSameStack(first));
    EXPECT_EQ(authored, memo.getGraftLayerAuthorCount());
}

// Swapping one input's stack swaps its graft layer alone, so the other input's subtree, and /T1,
// are not recomposed.
TEST(SceneReparentTest, EditingOneInputResyncsOnlyItsGraftedRoot)
{
    const ScenePayload card = stack({ makeLayer(kCard) });
    SceneGraftMemo memo;
    SceneReparentArgs args = graftUnderT1({ card, stack({ makeLayer(sphere(1.)) }) });
    setTranslateOverrides(&args, 1.);
    const ScenePayload before = reparent(args, &memo);

    SceneStage stage;
    ASSERT_TRUE(stage.isValid());
    EXPECT_TRUE(stage.sync(before));

    args.inputs[1] = stack({ makeLayer(sphere(2.)) });
    const ScenePayload after = reparent(args, &memo);
    const std::vector<SceneLayerEntry> beforeLayers = SceneOps::getLayers(before);
    const std::vector<SceneLayerEntry> afterLayers = SceneOps::getLayers(after);
    ASSERT_EQ(4u, beforeLayers.size());
    ASSERT_EQ(4u, afterLayers.size());
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(beforeLayers[i].layer, afterLayers[i].layer) << "layer " << i;
    }
    EXPECT_NE(beforeLayers[3].layer, afterLayers[3].layer);

    NoticeLog log(stage.getStage());
    EXPECT_TRUE(stage.sync(after));
    EXPECT_EQ(1, log.notices);
    ASSERT_FALSE(log.resynced.empty());
    for (const PXR_NS::SdfPath& path : log.resynced) {
        EXPECT_TRUE(path.HasPrefix(PXR_NS::SdfPath("/T1/Sphere1"))) << path;
    }
    double radius = 0.;
    EXPECT_TRUE(stage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/T1/Sphere1")).GetAttribute(PXR_NS::TfToken("radius")).Get(&radius));
    EXPECT_EQ(2., radius);
}

TEST(SceneReparentTest, OverridesEditResyncsOnlyTheDestinationsProperties)
{
    SceneGraftMemo memo;
    SceneReparentArgs args = graftUnderT1({ stack({ makeLayer(kCard) }), stack({ makeLayer(sphere(1.)) }) });
    setTranslateOverrides(&args, 1.);
    const ScenePayload before = reparent(args, &memo);

    SceneStage stage;
    ASSERT_TRUE(stage.isValid());
    EXPECT_TRUE(stage.sync(before));

    setTranslateOverrides(&args, 3.);
    const ScenePayload after = reparent(args, &memo);
    NoticeLog log(stage.getStage());
    EXPECT_TRUE(stage.sync(after));

    EXPECT_EQ(1, log.notices);
    ASSERT_FALSE(log.resynced.empty());
    for (const PXR_NS::SdfPath& path : log.resynced) {
        EXPECT_TRUE(path.IsPropertyPath()) << path;
        EXPECT_EQ(PXR_NS::SdfPath("/T1"), path.GetPrimPath()) << path;
    }
    PXR_NS::GfVec3d translate;
    EXPECT_TRUE(stage.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/T1")).GetAttribute(PXR_NS::TfToken("xformOp:translate")).Get(&translate));
    EXPECT_EQ(3., translate[0]);
}

TEST(SceneReparentTest, OverridesMayOnlyAuthorOvers)
{
    SceneGraftMemo memo;
    SceneReparentArgs args = graftUnderT1({ stack({ makeLayer(kCard) }) });
    args.overridesKey.nodeDigest = 7;
    args.authorOverrides = [](const SceneLayerEdit& edit, std::string* /*errorMessage*/) {
        return edit.layer->ImportFromString("#usda 1.0\ndef Xform \"T1\"\n{\n}\n");
    };
    ScenePayload output;
    std::string error;

    EXPECT_FALSE(SceneOps::reparentRoots(args, &memo, &output, NULL, &error));
    EXPECT_NE(std::string::npos, error.find("over")) << error;
}

// A render may hold a payload after its node re-authored or was deleted: the payload alone keeps
// the wrappers and the input layers they sublayer.
TEST(SceneReparentTest, PayloadHeldPastItsMemoStillComposes)
{
    ScenePayload held;
    std::string wrapperIdentifier;
    {
        SceneGraftMemo memo;
        held = reparent(graftUnderT1({ stack({ makeLayer(kCard) }), stack({ makeLayer(sphere(5.)) }) }), &memo);
        // The memo moves on to other inputs, then goes away with them.
        reparent(graftUnderT1({ stack({ makeLayer(sphere(6.)) }) }), &memo);
        const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(held);
        ASSERT_EQ(3u, layers.size());
        ASSERT_TRUE(layers[2].dependencies);
        ASSERT_EQ(1u, layers[2].dependencies->layers.size());
        wrapperIdentifier = layers[2].dependencies->layers[0]->GetIdentifier();
    }

    EXPECT_TRUE(PXR_NS::SdfLayer::Find(wrapperIdentifier));
    const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(held);
    ASSERT_TRUE(stage);
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/T1/Card1/Geo")).IsValid());
    double radius = 0.;
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/T1/Sphere1")).GetAttribute(PXR_NS::TfToken("radius")).Get(&radius));
    EXPECT_EQ(5., radius);

    // Restacked downstream, as a merge does, the entries keep what they reference.
    const ScenePayload merged = SceneOps::appendLayer(SceneOps::appendEntry(ScenePayload(), SceneOps::getLayers(held)[2]),
                                                      makeLayer("def Xform \"Other\"\n{\n}\n"), 99);
    held = ScenePayload();
    EXPECT_TRUE(PXR_NS::SdfLayer::Find(wrapperIdentifier));
    const PXR_NS::UsdStageRefPtr mergedStage = SceneOps::compose(merged);
    ASSERT_TRUE(mergedStage);
    EXPECT_TRUE(mergedStage->GetPrimAtPath(PXR_NS::SdfPath("/T1/Sphere1")).GetAttribute(PXR_NS::TfToken("radius")).Get(&radius));
    EXPECT_EQ(5., radius);
}

// An input whose layers run at another rate keeps its timing once grafted.
TEST(SceneReparentTest, GraftedSamplesKeepTheirInputsRate)
{
    const ScenePayload input = stack({ makeLayer("def Xform \"Card1\"\n{\n    double v.timeSamples = {\n        0: 0,\n        48: 48,\n    }\n}\n",
                                                 "(\n    timeCodesPerSecond = 48\n)\n") });
    SceneGraftMemo memo;
    const ScenePayload result = reparent(graftUnderT1({ input }), &memo);

    EXPECT_EQ(48., result.getMetadata().timeCodesPerSecond);
    const PXR_NS::UsdStageRefPtr flat = SceneOps::compose(input);
    const PXR_NS::UsdStageRefPtr grafted = SceneOps::compose(result);
    ASSERT_TRUE(flat);
    ASSERT_TRUE(grafted);
    for (double time : { 0., 24., 48. }) {
        double flatValue = -1.;
        double graftedValue = -2.;
        EXPECT_TRUE(flat->GetPrimAtPath(PXR_NS::SdfPath("/Card1")).GetAttribute(PXR_NS::TfToken("v")).Get(&flatValue, PXR_NS::UsdTimeCode(time)));
        EXPECT_TRUE(grafted->GetPrimAtPath(PXR_NS::SdfPath("/T1/Card1")).GetAttribute(PXR_NS::TfToken("v")).Get(&graftedValue, PXR_NS::UsdTimeCode(time)));
        EXPECT_EQ(flatValue, graftedValue) << "time " << time;
    }
}
