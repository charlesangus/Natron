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

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
PXR_NS::SdfLayerRefPtr
makeLayerDefining(const std::string& primPath)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ScenePayloadTest");
    PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, PXR_NS::SdfPath(primPath));

    if (prim) {
        prim->SetSpecifier(PXR_NS::SdfSpecifierDef);
        prim->SetTypeName("Xform");
    }

    return layer;
}

PXR_NS::SdfLayerRefPtr
makeLayerDefiningAll(const std::vector<std::string>& primPaths)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ScenePayloadTest");

    for (const std::string& primPath : primPaths) {
        PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, PXR_NS::SdfPath(primPath));
        if (prim) {
            prim->SetSpecifier(PXR_NS::SdfSpecifierDef);
            prim->SetTypeName("Xform");
        }
    }

    return layer;
}

SceneDrawRule
makeRule(const std::string& pattern,
         SceneDrawStateEnum state,
         const std::string& origin)
{
    SceneDrawRule rule;

    rule.pattern = pattern;
    rule.state = state;
    rule.origin = origin;

    return rule;
}

TEST(ScenePayloadTest, AWalkKeepingToItsLayersStopsAtPrimsOtherLayersAdded)
{
    PXR_NS::SdfLayerRefPtr file = makeLayerDefiningAll(std::vector<std::string> { "/World", "/World/a", "/World/a/geo" });
    PXR_NS::SdfLayerRefPtr other = makeLayerDefiningAll(std::vector<std::string> { "/World/added", "/World/added/geo" });
    PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("root.usda");
    root->InsertSubLayerPath(file->GetIdentifier());
    root->InsertSubLayerPath(other->GetIdentifier());
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(root);
    ASSERT_TRUE(stage);
    ASSERT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/added")).IsValid());

    SceneDrawRule levels = makeRule("/World", eSceneDrawStateBBox, "Read1");
    levels.levels = 2;
    levels.layers.push_back(file->GetIdentifier());
    SceneDrawStates::PolicyStates policy;
    ASSERT_TRUE(SceneOps::matchDrawRules(stage, SceneDrawRules(1, levels), &policy, std::function<bool()>()));
    EXPECT_EQ(eSceneDrawStateChildren, policy["/World"].state);
    EXPECT_EQ(eSceneDrawStateChildren, policy["/World/a"].state);
    EXPECT_EQ(eSceneDrawStateBBox, policy["/World/a/geo"].state);
    EXPECT_EQ(policy.end(), policy.find("/World/added"));
    EXPECT_EQ(policy.end(), policy.find("/World/added/geo"));

    // Without layers to keep to, the walk takes every prim.
    levels.layers.clear();
    ASSERT_TRUE(SceneOps::matchDrawRules(stage, SceneDrawRules(1, levels), &policy, std::function<bool()>()));
    EXPECT_EQ(eSceneDrawStateBBox, policy["/World/added/geo"].state);
}

TEST(ScenePayloadTest, RematchingUnderALoadedPayloadMatchesAsAWholeMatchDoes)
{
    PXR_NS::SdfLayerRefPtr asset = makeLayerDefiningAll(std::vector<std::string> { "/Asset", "/Asset/geo", "/Asset/geo/mesh" });
    PXR_NS::SdfLayerRefPtr file = makeLayerDefiningAll(std::vector<std::string> { "/World", "/World/a", "/World/b" });
    PXR_NS::SdfPrimSpecHandle b = file->GetPrimAtPath(PXR_NS::SdfPath("/World/b"));
    ASSERT_TRUE(b);
    b->GetPayloadList().Prepend(PXR_NS::SdfPayload(asset->GetIdentifier(), PXR_NS::SdfPath("/Asset")));
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(file, PXR_NS::UsdStage::LoadNone);
    ASSERT_TRUE(stage);

    SceneDrawRule levels = makeRule("/World", eSceneDrawStateBBox, "Read1");
    levels.levels = 3;
    const SceneDrawRules rules { makeRule("/World//geo", eSceneDrawStateHidden, "Expand1"), levels, makeRule("/World/b/geo/mesh", eSceneDrawStateFull, "Expand2") };
    SceneOps::SceneDrawRuleMatches matches;
    ASSERT_TRUE(SceneOps::matchDrawRules(stage, rules, &matches, std::function<bool()>()));
    SceneDrawStates::PolicyStates before;
    SceneOps::getDrawRulePolicy(rules, matches, &before);

    stage->Load(PXR_NS::SdfPath("/World/b"));
    ASSERT_TRUE(SceneOps::rematchDrawRulesUnder(stage, rules, std::vector<std::string>(1, "/World/b"), &matches, std::function<bool()>()));
    SceneDrawStates::PolicyStates rematched;
    SceneOps::getDrawRulePolicy(rules, matches, &rematched);
    SceneDrawStates::PolicyStates whole;
    ASSERT_TRUE(SceneOps::matchDrawRules(stage, rules, &whole, std::function<bool()>()));

    EXPECT_EQ(whole, rematched);
    EXPECT_NE(before, rematched);
    EXPECT_EQ(eSceneDrawStateFull, rematched["/World/b/geo/mesh"].state);

    stage->Unload(PXR_NS::SdfPath("/World/b"));
    ASSERT_TRUE(SceneOps::rematchDrawRulesUnder(stage, rules, std::vector<std::string>(1, "/World/b"), &matches, std::function<bool()>()));
    SceneOps::getDrawRulePolicy(rules, matches, &rematched);
    ASSERT_TRUE(SceneOps::matchDrawRules(stage, rules, &whole, std::function<bool()>()));
    EXPECT_EQ(whole, rematched);
}

std::string
exportComposed(const PXR_NS::UsdStageRefPtr& stage)
{
    std::string text;

    if (stage) {
        stage->ExportToString(&text, false);
    }

    return text;
}

bool
hasNoSessionOpinions(const PXR_NS::UsdStageRefPtr& stage)
{
    const PXR_NS::SdfLayerHandle session = stage->GetSessionLayer();

    return !session || session->IsEmpty();
}

// The #include lines of a source file, so a test can tell which headers it pulls in.
std::vector<std::string>
readIncludeLines(const std::string& path)
{
    std::vector<std::string> includes;
    std::ifstream file(path.c_str());
    std::string line;

    while (std::getline(file, line)) {
        if (line.find("#include") != std::string::npos) {
            includes.push_back(line);
        }
    }

    return includes;
}
} // anonymous namespace

TEST(ScenePayloadTest, EmptyPayloadHasNoLayersAndFallbackMetadata)
{
    ScenePayload payload;

    EXPECT_TRUE(payload.isEmpty());
    EXPECT_EQ((std::size_t)0, payload.getNumLayers());
    EXPECT_EQ((U64)0, payload.getStackHash());
    EXPECT_EQ(SceneMetadata(), payload.getMetadata());
    EXPECT_TRUE(SceneOps::getLayers(payload).empty());
}

TEST(ScenePayloadTest, AppendingALayerChangesTheStackHash)
{
    const ScenePayload empty;
    const ScenePayload one = SceneOps::appendLayer(empty, makeLayerDefining("/A"), 11);
    const ScenePayload two = SceneOps::appendLayer(one, makeLayerDefining("/B"), 22);

    EXPECT_EQ((std::size_t)1, one.getNumLayers());
    EXPECT_EQ((std::size_t)2, two.getNumLayers());
    EXPECT_NE((U64)0, one.getStackHash());
    EXPECT_NE(empty.getStackHash(), one.getStackHash());
    EXPECT_NE(one.getStackHash(), two.getStackHash());

    // The base is a value: appending to it left it as it was.
    EXPECT_EQ((std::size_t)1, one.getNumLayers());
}

TEST(ScenePayloadTest, IdenticalStacksHashIdentically)
{
    const ScenePayload first = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), makeLayerDefining("/A"), 11), makeLayerDefining("/B"), 22);
    const ScenePayload second = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), makeLayerDefining("/A"), 11), makeLayerDefining("/B"), 22);

    EXPECT_EQ(first.getStackHash(), second.getStackHash());
    // Equal content, but other layer objects.
    EXPECT_FALSE(first.isSameStack(second));
    EXPECT_TRUE(first.isSameStack(first));

    const ScenePayload swapped = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), makeLayerDefining("/B"), 22), makeLayerDefining("/A"), 11);
    EXPECT_NE(first.getStackHash(), swapped.getStackHash());
}

TEST(ScenePayloadTest, LayersAreListedWeakestFirst)
{
    PXR_NS::SdfLayerRefPtr weak = makeLayerDefining("/A");
    PXR_NS::SdfLayerRefPtr strong = makeLayerDefining("/B");
    const ScenePayload payload = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), weak, 1), strong, 2);

    const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(payload);
    ASSERT_EQ((std::size_t)2, layers.size());
    EXPECT_EQ(weak, layers[0].layer);
    EXPECT_EQ((U64)1, layers[0].hash);
    EXPECT_EQ(strong, layers[1].layer);
    EXPECT_EQ((U64)2, layers[1].hash);
}

// For each field the strongest layer authoring it wins, and a layer authoring nothing leaves
// the weaker layers' values alone.
TEST(ScenePayloadTest, MetadataResolvesStrongestAuthoredFirst)
{
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();

    PXR_NS::SdfLayerRefPtr weak = makeLayerDefining("/A");
    weak->SetField(root, PXR_NS::UsdGeomTokens->upAxis, PXR_NS::UsdGeomTokens->z);
    weak->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, 1.);
    weak->SetDefaultPrim(PXR_NS::TfToken("A"));
    weak->SetTimeCodesPerSecond(30.);

    PXR_NS::SdfLayerRefPtr strong = makeLayerDefining("/B");
    strong->SetDefaultPrim(PXR_NS::TfToken("B"));

    PXR_NS::SdfLayerRefPtr silent = makeLayerDefining("/C");

    const ScenePayload payload = SceneOps::appendLayer(SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), weak, 1), strong, 2), silent, 3);
    const SceneMetadata& metadata = payload.getMetadata();

    EXPECT_EQ(SceneMetadata::eUpAxisZ, metadata.upAxis);
    EXPECT_EQ(1., metadata.metersPerUnit);
    EXPECT_EQ(std::string("B"), metadata.defaultPrim);
    EXPECT_EQ(30., metadata.timeCodesPerSecond);
}

// Composition sublayers the stack strongest first, so a stronger opinion wins, and carries the
// resolved metadata onto the stage.
TEST(ScenePayloadTest, ComposedStageHoldsEveryLayerWithTheStrongestOpinionWinning)
{
    const PXR_NS::SdfPath primPath("/A");
    const PXR_NS::TfToken attributeName("natron:value");

    PXR_NS::SdfLayerRefPtr weak = makeLayerDefining("/A");
    PXR_NS::SdfAttributeSpecHandle weakAttribute = PXR_NS::SdfAttributeSpec::New(weak->GetPrimAtPath(primPath), attributeName, PXR_NS::SdfValueTypeNames->Int);
    ASSERT_TRUE(bool(weakAttribute));
    weakAttribute->SetDefaultValue(PXR_NS::VtValue(1));
    weak->SetField(PXR_NS::SdfPath::AbsoluteRootPath(), PXR_NS::UsdGeomTokens->upAxis, PXR_NS::UsdGeomTokens->z);

    PXR_NS::SdfLayerRefPtr strong = SceneOps::createNodeLayer("ScenePayloadTest");
    PXR_NS::SdfPrimSpecHandle over = PXR_NS::SdfCreatePrimInLayer(strong, primPath);
    ASSERT_TRUE(bool(over));
    PXR_NS::SdfAttributeSpecHandle strongAttribute = PXR_NS::SdfAttributeSpec::New(over, attributeName, PXR_NS::SdfValueTypeNames->Int);
    ASSERT_TRUE(bool(strongAttribute));
    strongAttribute->SetDefaultValue(PXR_NS::VtValue(2));
    PXR_NS::SdfPrimSpecHandle other = PXR_NS::SdfCreatePrimInLayer(strong, PXR_NS::SdfPath("/B"));
    ASSERT_TRUE(bool(other));
    other->SetSpecifier(PXR_NS::SdfSpecifierDef);

    const ScenePayload payload = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), weak, 1), strong, 2);
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));

    PXR_NS::UsdPrim prim = stage->GetPrimAtPath(primPath);
    ASSERT_TRUE(prim.IsValid());
    int value = 0;
    EXPECT_TRUE(prim.GetAttribute(attributeName).Get(&value));
    EXPECT_EQ(2, value);
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/B")).IsValid());

    PXR_NS::TfToken upAxis;
    EXPECT_TRUE(stage->GetMetadata(PXR_NS::UsdGeomTokens->upAxis, &upAxis));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->z, upAxis);
}

// USD stays behind Engine/Nodes/Scene/: the transport type and the engine headers that carry
// it include no pxr header.
TEST(ScenePayloadTest, CoreHeadersIncludeNoPxrHeader)
{
    const char* headers[] = {
        "/ScenePayload.h",
        "/EffectInstance.h",
        "/Nodes/NativeEffectBase.h",
        "/Nodes/Scene/SceneLayerMemo.h",
    };

    for (const char* header : headers) {
        const std::string path = std::string(NATRON_TESTS_ENGINE_DIR) + header;
        const std::vector<std::string> includes = readIncludeLines(path);

        ASSERT_FALSE(includes.empty()) << path << " could not be read";
        for (std::size_t i = 0; i < includes.size(); ++i) {
            EXPECT_EQ(std::string::npos, includes[i].find("pxr")) << path << ": " << includes[i];
            EXPECT_EQ(std::string::npos, includes[i].find("UsdIncludes")) << path << ": " << includes[i];
        }
    }
}

TEST(ScenePayloadTest, DrawRulesLeaveTheStackAndItsHashAlone)
{
    const ScenePayload plain = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), makeLayerDefining("/A"), 11), makeLayerDefining("/B"), 22);
    const ScenePayload ruled = plain.withDrawRules(SceneDrawRules(1, makeRule("/A", eSceneDrawStateBBox, "Policy1")));
    const ScenePayload otherRules = plain.withDrawRules(SceneDrawRules(1, makeRule("/B", eSceneDrawStateHidden, "Policy2")));

    EXPECT_TRUE(plain.getDrawRules().empty());
    ASSERT_EQ((std::size_t)1, ruled.getDrawRules().size());
    EXPECT_EQ(std::string("/A"), ruled.getDrawRules()[0].pattern);
    EXPECT_EQ(plain.getStackHash(), ruled.getStackHash());
    EXPECT_EQ(ruled.getStackHash(), otherRules.getStackHash());
    EXPECT_TRUE(plain.isSameStack(ruled));
    EXPECT_TRUE(ruled.isSameStack(otherRules));
    EXPECT_EQ(plain.getNumLayers(), ruled.getNumLayers());
    EXPECT_TRUE(ruled.withDrawRules(SceneDrawRules()).getDrawRules().empty());

    // A layer appended to a payload with rules is a new node's output: evaluateScene() hands the
    // rules on, not the stack.
    EXPECT_TRUE(SceneOps::appendLayer(ruled, makeLayerDefining("/C"), 33).getDrawRules().empty());
}

TEST(ScenePayloadTest, AppendingDrawRulesKeepsOrderAndTheFirstOfARepeat)
{
    const SceneDrawRule a = makeRule("/A", eSceneDrawStateBBox, "Policy1");
    const SceneDrawRule b = makeRule("/B", eSceneDrawStateHidden, "Policy2");
    const SceneDrawRule c = makeRule("/A", eSceneDrawStateFull, "Policy3");
    SceneDrawRules rules;

    appendSceneDrawRules(SceneDrawRules { a, b }, &rules);
    appendSceneDrawRules(SceneDrawRules { b, c, a }, &rules);

    ASSERT_EQ((std::size_t)3, rules.size());
    EXPECT_EQ(a, rules[0]);
    EXPECT_EQ(b, rules[1]);
    EXPECT_EQ(c, rules[2]);
}

TEST(ScenePayloadTest, AppendingDrawRulesIntoNoneTakesThemAsTheyAre)
{
    const SceneDrawRule a = makeRule("/A", eSceneDrawStateBBox, "Policy1");
    const SceneDrawRule b = makeRule("/B", eSceneDrawStateHidden, "Policy1");
    SceneDrawRules rules;

    appendSceneDrawRules(SceneDrawRules { a, b, a }, &rules);

    EXPECT_EQ((SceneDrawRules { a, b, a }), rules);
}

TEST(ScenePayloadTest, AppendingDrawRulesKeepsTheirOwnRepeatsBehindTheTargetsRules)
{
    const SceneDrawRule up = makeRule("/U", eSceneDrawStateFull, "Policy1");
    const SceneDrawRule a = makeRule("/A", eSceneDrawStateBBox, "Policy2");
    const SceneDrawRule b = makeRule("/A/B", eSceneDrawStateHidden, "Policy2");
    SceneDrawRules rules { up };

    appendSceneDrawRules(SceneDrawRules { a, b, a }, &rules);

    // The later a still decides what b would: the repeat is kept where it was.
    EXPECT_EQ((SceneDrawRules { up, a, b, a }), rules);

    appendSceneDrawRules(SceneDrawRules { up, a }, &rules);

    EXPECT_EQ((SceneDrawRules { up, a, b, a }), rules);
}

TEST(ScenePayloadTest, AppendingManyDrawRulesLeavesOutEveryRepeat)
{
    SceneDrawRules first;
    SceneDrawRules second;
    for (int i = 0; i < 5000; ++i) {
        first.push_back(makeRule("/P" + std::to_string(i), eSceneDrawStateBBox, "Policy1"));
        second.push_back(makeRule("/P" + std::to_string(i * 2), eSceneDrawStateBBox, "Policy1"));
    }
    SceneDrawRule walked = makeRule("/P1", eSceneDrawStateBBox, "Policy1");
    walked.layers.push_back("file.usda");
    second.push_back(walked);
    SceneDrawRules rules;

    appendSceneDrawRules(first, &rules);
    appendSceneDrawRules(second, &rules);

    // The even paths below 5000 repeat; the rule keeping to a layer differs from the plain one.
    ASSERT_EQ((std::size_t)(5000 + 2500 + 1), rules.size());
    EXPECT_EQ(first[4999], rules[4999]);
    EXPECT_EQ(std::string("/P5000"), rules[5000].pattern);
    EXPECT_EQ(walked, rules.back());
}

TEST(ScenePayloadTest, DrawRulesLeaveNoTraceOnAComposedOrSyncedStage)
{
    const ScenePayload plain = SceneOps::appendLayer(ScenePayload(), makeLayerDefiningAll(std::vector<std::string> { "/World", "/World/a" }), 11);
    const ScenePayload ruled = plain.withDrawRules(SceneDrawRules { makeRule("/World/a", eSceneDrawStateHidden, "Policy1"), makeRule("/World//*", eSceneDrawStateBBox, "Policy2") });

    const PXR_NS::UsdStageRefPtr composedPlain = SceneOps::compose(plain);
    const PXR_NS::UsdStageRefPtr composedRuled = SceneOps::compose(ruled);
    ASSERT_TRUE(composedPlain && composedRuled);
    EXPECT_EQ(exportComposed(composedPlain), exportComposed(composedRuled));
    EXPECT_TRUE(hasNoSessionOpinions(composedRuled));
    std::string plainRoot;
    std::string ruledRoot;
    composedPlain->GetRootLayer()->ExportToString(&plainRoot);
    composedRuled->GetRootLayer()->ExportToString(&ruledRoot);
    EXPECT_EQ(plainRoot, ruledRoot);

    // The stage a render or a viewer keeps: rules alone never compose it again.
    SceneStage stage;
    ASSERT_TRUE(stage.isValid());
    EXPECT_TRUE(stage.sync(plain));
    const std::string synced = exportComposed(stage.getStage());
    EXPECT_FALSE(stage.sync(ruled));
    EXPECT_EQ(synced, exportComposed(stage.getStage()));
    EXPECT_TRUE(hasNoSessionOpinions(stage.getStage()));
    std::string root;
    stage.getStage()->GetRootLayer()->ExportToString(&root);
    EXPECT_EQ(std::string::npos, root.find("Policy1"));
    EXPECT_EQ(std::string::npos, root.find("Policy2"));
}

TEST(ScenePayloadTest, ViewerStatesBeatLaterRulesWhichBeatEarlierOnesWhichBeatTheDefault)
{
    const SceneDrawRules rules {
        makeRule("/World/a /World/b", eSceneDrawStateBBox, "Early"),
        makeRule("/World/b", eSceneDrawStateHidden, "Late"),
    };
    SceneDrawStates::PolicyStates policy;

    // Plain paths need no stage.
    ASSERT_TRUE(SceneOps::matchDrawRules(PXR_NS::UsdStageRefPtr(), rules, &policy, std::function<bool()>()));
    ASSERT_EQ((std::size_t)2, policy.size());
    EXPECT_EQ(eSceneDrawStateBBox, policy["/World/a"].state);
    EXPECT_EQ(std::string("Early"), policy["/World/a"].origin);
    EXPECT_EQ(eSceneDrawStateHidden, policy["/World/b"].state);
    EXPECT_EQ(std::string("Late"), policy["/World/b"].origin);

    SceneDrawStates states;
    EXPECT_FALSE(states.replacePolicyStates(policy).isEmpty());
    SceneDrawResolved resolved = states.resolve("/World/a");
    EXPECT_EQ(eSceneDrawStateBBox, resolved.state);
    EXPECT_EQ(eSceneDrawOriginGraphPolicy, resolved.origin);
    resolved = states.resolve("/World/b");
    EXPECT_EQ(eSceneDrawStateHidden, resolved.state);
    EXPECT_EQ(eSceneDrawOriginGraphPolicy, resolved.origin);
    resolved = states.resolve("/World/c");
    EXPECT_EQ(eSceneDrawStateFull, resolved.state);
    EXPECT_EQ(eSceneDrawOriginDefault, resolved.origin);

    states.setState("/World/b", eSceneDrawStateFull);
    resolved = states.resolve("/World/b");
    EXPECT_EQ(eSceneDrawStateFull, resolved.state);
    EXPECT_EQ(eSceneDrawOriginViewer, resolved.origin);

    // The same policy again changes nothing; the viewer's state outlives a policy change.
    EXPECT_TRUE(states.replacePolicyStates(policy).isEmpty());
    EXPECT_FALSE(states.replacePolicyStates(SceneDrawStates::PolicyStates()).isEmpty());
    EXPECT_EQ(eSceneDrawOriginDefault, states.resolve("/World/a").origin);
    EXPECT_EQ(eSceneDrawOriginViewer, states.resolve("/World/b").origin);
}

TEST(ScenePayloadTest, ALaterFullRuleOpensTheEarlierBoxesAboveIt)
{
    SceneDrawStates::PolicyStates policy;

    ASSERT_TRUE(SceneOps::matchDrawRules(PXR_NS::UsdStageRefPtr(),
                                         SceneDrawRules { makeRule("/World", eSceneDrawStateBBox, "Early"), makeRule("/World/a/b", eSceneDrawStateFull, "Late") },
                                         &policy,
                                         std::function<bool()>()));
    EXPECT_EQ(eSceneDrawStateChildren, policy["/World"].state);
    EXPECT_EQ(std::string("Late"), policy["/World"].origin);
    EXPECT_EQ(eSceneDrawStateFull, policy["/World/a/b"].state);

    SceneDrawStates states;
    states.replacePolicyStates(policy);
    EXPECT_EQ(eSceneDrawStateFull, states.resolve("/World/a/b").state);
    EXPECT_EQ(eSceneDrawStateBBox, states.resolve("/World/other").state);
}

TEST(ScenePayloadTest, DrawRulePatternsMatchTheStageAndOneMatchingNothingDoesNothing)
{
    PXR_NS::SdfLayerRefPtr layer = makeLayerDefiningAll(std::vector<std::string> { "/World", "/World/a", "/World/a/geo", "/World/b", "/World/b/geo" });
    PXR_NS::SdfPrimSpecHandle asset = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/b"));
    ASSERT_TRUE(asset);
    asset->SetKind(PXR_NS::KindTokens->component);
    const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(SceneOps::appendLayer(ScenePayload(), layer, 11));
    ASSERT_TRUE(stage);

    SceneDrawStates::PolicyStates policy;
    ASSERT_TRUE(SceneOps::matchDrawRules(stage, SceneDrawRules { makeRule("/World/*", eSceneDrawStateBBox, "Policy1") }, &policy, std::function<bool()>()));
    ASSERT_EQ((std::size_t)2, policy.size());
    EXPECT_EQ(eSceneDrawStateBBox, policy["/World/a"].state);
    EXPECT_EQ(eSceneDrawStateBBox, policy["/World/b"].state);

    ASSERT_TRUE(SceneOps::matchDrawRules(stage, SceneDrawRules { makeRule("/Nowhere//*", eSceneDrawStateHidden, "Policy1"), makeRule("((", eSceneDrawStateHidden, "Policy1") }, &policy, std::function<bool()>()));
    EXPECT_TRUE(policy.empty());

    SceneDrawRule levels = makeRule("/World", eSceneDrawStateBBox, "Policy1");
    levels.levels = 1;
    ASSERT_TRUE(SceneOps::matchDrawRules(stage, SceneDrawRules(1, levels), &policy, std::function<bool()>()));
    ASSERT_EQ((std::size_t)3, policy.size());
    EXPECT_EQ(eSceneDrawStateChildren, policy["/World"].state);
    EXPECT_EQ(eSceneDrawStateBBox, policy["/World/a"].state);
    EXPECT_EQ(eSceneDrawStateBBox, policy["/World/b"].state);

    SceneDrawRule toKind = makeRule("/World", eSceneDrawStateHidden, "Policy1");
    toKind.toKind = "model";
    ASSERT_TRUE(SceneOps::matchDrawRules(stage, SceneDrawRules(1, toKind), &policy, std::function<bool()>()));
    ASSERT_EQ((std::size_t)2, policy.size());
    EXPECT_EQ(eSceneDrawStateChildren, policy["/World"].state);
    EXPECT_EQ(eSceneDrawStateHidden, policy["/World/b"].state);

    EXPECT_FALSE(SceneOps::drawRulesNeedStage(SceneDrawRules { makeRule("/World/a /World/b", eSceneDrawStateBBox, "Policy1") }));
    EXPECT_TRUE(SceneOps::drawRulesNeedStage(SceneDrawRules { makeRule("/World/*", eSceneDrawStateBBox, "Policy1") }));
    EXPECT_TRUE(SceneOps::drawRulesNeedStage(SceneDrawRules(1, levels)));
}
